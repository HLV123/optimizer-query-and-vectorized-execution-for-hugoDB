/* phys_executor.c — Execute PhysicalPlan against DiskDB
 *
 * Iterator model: each operator has open/next/close semantics,
 * but since Hugo DB scans are bulk (callback-based via ddb_scan),
 * we materialize each operator's output into a temp Document* array,
 * then feed it to the next operator.
 *
 * This is the "volcano with materialization" model — simpler than
 * true pipelining but correct and easy to debug.
 *
 * NOTE: condition evaluation is duplicated from executor_disk.c.
 * In a production system this would be shared via an internal header.
 */
#include "phys_executor.h"
#include "collection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== Result buffer (materialized rows) ===== */
#define PHYS_MAX_ROWS 100000

typedef struct RowSet {
    Document **rows;
    int        count;
    int        cap;
    int        owned; /* 1 = this RowSet owns the docs (must free) */
} RowSet;

static RowSet rowset_new(int owned) {
    RowSet rs;
    rs.count  = 0;
    rs.cap    = 256;
    rs.owned  = owned;
    rs.rows   = (Document**)malloc(rs.cap * sizeof(Document*));
    return rs;
}


static void rowset_push(RowSet *rs, Document *d) {
    if (rs->count >= PHYS_MAX_ROWS) return;
    if (rs->count >= rs->cap) {
        rs->cap *= 2;
        rs->rows = (Document**)realloc(rs->rows, rs->cap * sizeof(Document*));
    }
    if (rs->rows) rs->rows[rs->count++] = d;
}

static void rowset_free(RowSet *rs) {
    if (rs->owned && rs->rows) {
        for (int i = 0; i < rs->count; i++)
            if (rs->rows[i]) doc_free(rs->rows[i]);
    }
    free(rs->rows);
    rs->rows  = NULL;
    rs->count = 0;
}

/* ===== Condition evaluation (copy from executor_disk.c) ===== */

static int pe_value_compare(const Value *a, const Value *b) {
    if (a->type != b->type) {
        if (a->type == VAL_NUM && b->type == VAL_NUM) return 0;
        return -2;
    }
    if (a->type == VAL_NUM) {
        if (a->num < b->num) return -1;
        if (a->num > b->num) return  1;
        return 0;
    }
    if (a->type == VAL_STR) return strcmp(a->str, b->str);
    return -2;
}

static int pe_eval_cmp(const Condition *c, const Document *d) {
    Value dv;
    if (c->type == COND_EXISTS)
        return doc_get_field(d, c->field, NULL) == 0;
    if (doc_get_field(d, c->field, &dv) != 0) return 0;
    int cmp = pe_value_compare(&dv, &c->value);
    switch (c->op) {
    case TOK_OP_BG:  return cmp == 0;
    case TOK_OP_KC:  return cmp != 0 && cmp != -2;
    case TOK_OP_LH:  return cmp == -1;
    case TOK_OP_BH:  return cmp == 1;
    case TOK_OP_LHB: return cmp == -1 || cmp == 0;
    case TOK_OP_BHB: return cmp == 1  || cmp == 0;
    case TOK_OP_XAU:
        if (dv.type != VAL_STR || c->value.type != VAL_STR) return 0;
        return strstr(dv.str, c->value.str) != NULL;
    default: return 0;
    }
}

static int pe_eval_in(const Condition *c, const Document *d) {
    Value dv;
    if (doc_get_field(d, c->field, &dv) != 0) return 0;
    int found = 0;
    for (int i = 0; i < c->n_values; i++)
        if (pe_value_compare(&dv, &c->values[i]) == 0) { found = 1; break; }
    return (c->op == TOK_OP_TG) ? found : !found;
}

static int pe_eval_condition(const Condition *c, const Document *d) {
    if (!c) return 1;
    switch (c->type) {
    case COND_AND:    return pe_eval_condition(c->left, d) && pe_eval_condition(c->right, d);
    case COND_OR:     return pe_eval_condition(c->left, d) || pe_eval_condition(c->right, d);
    case COND_NOT:    return !pe_eval_condition(c->left, d);
    case COND_IN:     return pe_eval_in(c, d);
    case COND_CMP:
    case COND_EXISTS: return pe_eval_cmp(c, d);
    default:          return 0;
    }
}

/* ===== Sort comparator ===== */
static const SortField *g_pe_sort_head = NULL;

static int pe_compare_docs(const void *a, const void *b) {
    const Document *da = *(const Document*const*)a;
    const Document *db = *(const Document*const*)b;
    for (const SortField *s = g_pe_sort_head; s; s = s->next) {
        Value va, vb;
        int ha = doc_get_field(da, s->field, &va) == 0;
        int hb = doc_get_field(db, s->field, &vb) == 0;
        if (!ha && !hb) continue;
        if (!ha) return s->descending ? 1 : -1;
        if (!hb) return s->descending ? -1 : 1;
        int c = pe_value_compare(&va, &vb);
        if (c == -2) continue;
        if (c != 0) return s->descending ? -c : c;
    }
    return 0;
}

/* ===== Scan callback context ===== */
typedef struct { RowSet *rs; } ScanFillCtx;

static void scan_fill_visit(uint64_t id, Document *d, void *ctx_) {
    (void)id;
    ScanFillCtx *sc = (ScanFillCtx*)ctx_;
    rowset_push(sc->rs, doc_clone(d));
}

/* ===== Hash join helpers ===== */
#define HJ_BUCKETS 4096

typedef struct HJEntry {
    Document        *doc;
    struct HJEntry  *next;
} HJEntry;

typedef struct {
    HJEntry *buckets[HJ_BUCKETS];
    int      count;
} HashTable;

static uint64_t hj_hash_value(const Value *v) {
    if (v->type == VAL_NUM) {
        uint64_t bits;
        memcpy(&bits, &v->num, sizeof(bits));
        return bits ^ (bits >> 17) ^ (bits >> 31);
    }
    if (v->type == VAL_STR) {
        uint64_t h = 14695981039346656037ULL;
        for (const char *p = v->str; *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        return h;
    }
    return 0;
}

static HashTable* ht_build(const RowSet *rs, const char *key_field, Arena *arena) {
    HashTable *ht = (HashTable*)arena_alloc(arena, sizeof(HashTable));
    if (!ht) return NULL;
    memset(ht, 0, sizeof(HashTable));

    for (int i = 0; i < rs->count; i++) {
        Value kv;
        if (doc_get_field(rs->rows[i], key_field, &kv) != 0) continue;
        uint64_t h = hj_hash_value(&kv) % HJ_BUCKETS;
        HJEntry *e = (HJEntry*)arena_alloc(arena, sizeof(HJEntry));
        if (!e) continue;
        e->doc  = rs->rows[i];
        e->next = ht->buckets[h];
        ht->buckets[h] = e;
        ht->count++;
    }
    return ht;
}

/* ===== Aggregate helpers ===== */
typedef struct AggBucket {
    Value   key;
    int     count;
    double  sums[16], mins[16], maxs[16];
    int     inited[16];
    struct AggBucket *next;
} AggBucket;

/* ===== Main recursive executor ===== */

static RowSet phys_exec_node(DiskDB *db, const PhysicalPlan *plan, Arena *arena);

static RowSet exec_seq_scan(DiskDB *db, const PhysicalPlan *plan) {
    RowSet rs = rowset_new(1);
    DiskColl *c = ddb_get_coll(db, plan->seq_scan.collection_name);
    if (!c) return rs;
    ScanFillCtx ctx = { &rs };
    ddb_scan(db, c, scan_fill_visit, &ctx);
    return rs;
}

static RowSet exec_index_scan(DiskDB *db, const PhysicalPlan *plan) {
    /* MVP: fall back to seq scan + filter — true index traversal is a Phase 8b goal.
     * The cost model already used index cost to make the decision; execution-wise
     * we do the same work as seq scan here. */
    RowSet rs = rowset_new(1);
    DiskColl *c = ddb_get_coll(db, plan->index_scan.collection_name);
    if (!c) return rs;
    ScanFillCtx ctx = { &rs };
    ddb_scan(db, c, scan_fill_visit, &ctx);
    return rs;
}

static RowSet exec_filter(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet child = phys_exec_node(db, plan->left, arena);
    RowSet out   = rowset_new(1);
    const Condition *pred = plan->filter.predicate;

    for (int i = 0; i < child.count; i++) {
        if (pe_eval_condition(pred, child.rows[i]))
            rowset_push(&out, child.rows[i]);
        else
            doc_free(child.rows[i]);
    }
    /* child.rows array is freed but docs are now owned by out (or freed above) */
    free(child.rows);
    return out;
}

static RowSet exec_nested_loop_join(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet left  = phys_exec_node(db, plan->left,  arena);
    RowSet right = phys_exec_node(db, plan->right, arena);
    RowSet out   = rowset_new(1);

    for (int i = 0; i < left.count; i++) {
        Value lv;
        if (doc_get_field(left.rows[i], plan->join.left_col, &lv) != 0) continue;
        for (int j = 0; j < right.count; j++) {
            Value rv;
            if (doc_get_field(right.rows[j], plan->join.right_col, &rv) != 0) continue;
            if (pe_value_compare(&lv, &rv) == 0) {
                /* Merge: clone left, embed right fields */
                Document *merged = doc_clone(left.rows[i]);
                for (KVPair *kv = right.rows[j]->pairs; kv; kv = kv->next)
                    doc_set_field(merged, kv->key, kv->value);
                rowset_push(&out, merged);
                break; /* first match only (like original executor) */
            }
        }
    }
    rowset_free(&left);
    rowset_free(&right);
    return out;
}

static RowSet exec_hash_join(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet build = phys_exec_node(db, plan->right, arena); /* smaller side = right */
    RowSet probe = phys_exec_node(db, plan->left,  arena);
    RowSet out   = rowset_new(1);

    HashTable *ht = ht_build(&build, plan->join.right_col, arena);
    if (!ht) {
        rowset_free(&build);
        rowset_free(&probe);
        return out;
    }

    for (int i = 0; i < probe.count; i++) {
        Value pv;
        if (doc_get_field(probe.rows[i], plan->join.left_col, &pv) != 0) continue;
        uint64_t h = hj_hash_value(&pv) % HJ_BUCKETS;
        for (HJEntry *e = ht->buckets[h]; e; e = e->next) {
            Value bv;
            if (doc_get_field(e->doc, plan->join.right_col, &bv) != 0) continue;
            if (pe_value_compare(&pv, &bv) == 0) {
                Document *merged = doc_clone(probe.rows[i]);
                for (KVPair *kv = e->doc->pairs; kv; kv = kv->next)
                    doc_set_field(merged, kv->key, kv->value);
                rowset_push(&out, merged);
                break;
            }
        }
    }
    rowset_free(&build);
    rowset_free(&probe);
    return out;
}

static RowSet exec_sort_merge_join(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    /* For Hugo DB document sizes, just fall back to hash join implementation
     * (sort-merge join needs sorted inputs which we'd need to verify/sort).
     * This keeps correctness. A true SMJ would sort both sides by join key first. */
    return exec_hash_join(db, plan, arena);
}

static RowSet exec_sort(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet child = phys_exec_node(db, plan->left, arena);
    if (child.count > 1 && plan->sort.fields) {
        g_pe_sort_head = plan->sort.fields;
        qsort(child.rows, child.count, sizeof(Document*), pe_compare_docs);
        g_pe_sort_head = NULL;
    }
    return child;
}

static RowSet exec_limit(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet child = phys_exec_node(db, plan->left, arena);
    int skip  = plan->limit.skip;
    int lim   = plan->limit.limit;

    /* Apply skip */
    if (skip > 0) {
        int to_skip = skip < child.count ? skip : child.count;
        for (int i = 0; i < to_skip; i++) doc_free(child.rows[i]);
        int new_n = child.count - to_skip;
        memmove(child.rows, child.rows + to_skip, new_n * sizeof(Document*));
        child.count = new_n;
    }

    /* Apply limit */
    if (lim >= 0 && child.count > lim) {
        for (int i = lim; i < child.count; i++) doc_free(child.rows[i]);
        child.count = lim;
    }
    return child;
}

static RowSet exec_hash_aggregate(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    RowSet child = phys_exec_node(db, plan->left, arena);
    RowSet out   = rowset_new(1);

    AggBucket *groups = NULL;
    int n_groups = 0;

    for (int i = 0; i < child.count; i++) {
        Document *d = child.rows[i];
        Value gk;
        if (doc_get_field(d, plan->aggregate.group_by_field, &gk) != 0)
            gk.type = VAL_NULL;

        AggBucket *g = NULL;
        for (AggBucket *p = groups; p; p = p->next)
            if (pe_value_compare(&p->key, &gk) == 0) { g = p; break; }
        if (!g) {
            g = (AggBucket*)calloc(1, sizeof(AggBucket));
            if (!g) break;
            g->key  = gk;
            g->next = groups;
            groups  = g;
            n_groups++;
        }
        g->count++;
        for (size_t a = 0; a < plan->aggregate.n_aggs; a++) {
            Value av;
            if (doc_get_field(d, plan->aggregate.aggs[a].field, &av) != 0) continue;
            double v = (av.type == VAL_NUM) ? av.num : 0;
            g->sums[a] += v;
            if (!g->inited[a] || v < g->mins[a]) g->mins[a] = v;
            if (!g->inited[a] || v > g->maxs[a]) g->maxs[a] = v;
            g->inited[a] = 1;
        }
    }

    /* Build result documents */
    for (AggBucket *g = groups; g && out.count < PHYS_MAX_ROWS; g = g->next) {
        Document *doc = (Document*)calloc(1, sizeof(Document));
        doc_set_field(doc, plan->aggregate.group_by_field, g->key);
        for (size_t a = 0; a < plan->aggregate.n_aggs; a++) {
            Value v; memset(&v, 0, sizeof(v)); v.type = VAL_NUM;
            switch (plan->aggregate.aggs[a].func) {
            case TOK_POU: v.num = g->count;  break;
            case TOK_SEP: v.num = g->sums[a]; break;
            case TOK_AWR: v.num = g->count > 0 ? g->sums[a] / g->count : 0; break;
            case TOK_MIE: v.num = g->mins[a]; break;
            case TOK_MAF: v.num = g->maxs[a]; break;
            default: break;
            }
            doc_set_field(doc, plan->aggregate.aggs[a].out_name, v);
        }
        rowset_push(&out, doc);
    }

    /* Free groups */
    AggBucket *g = groups;
    while (g) { AggBucket *n = g->next; free(g); g = n; }
    rowset_free(&child);
    return out;
}

/* ===== Main dispatch ===== */

static RowSet phys_exec_node(DiskDB *db, const PhysicalPlan *plan, Arena *arena) {
    if (!plan) {
        /* Return empty rowset — caller must free .rows */
        RowSet empty = rowset_new(1);
        return empty;
    }

    switch (plan->type) {
    case POP_SEQ_SCAN:         return exec_seq_scan(db, plan);
    case POP_INDEX_SCAN:       return exec_index_scan(db, plan);
    case POP_FILTER:           return exec_filter(db, plan, arena);
    case POP_NESTED_LOOP_JOIN: return exec_nested_loop_join(db, plan, arena);
    case POP_HASH_JOIN:        return exec_hash_join(db, plan, arena);
    case POP_SORT_MERGE_JOIN:  return exec_sort_merge_join(db, plan, arena);
    case POP_SORT:             return exec_sort(db, plan, arena);
    case POP_LIMIT:            return exec_limit(db, plan, arena);
    case POP_HASH_AGGREGATE:
    case POP_STREAM_AGGREGATE: return exec_hash_aggregate(db, plan, arena);
    case POP_PROJECT:          /* pass-through for now */
        if (plan->left) return phys_exec_node(db, plan->left, arena);
        /* fall through to default */
    default: {
        RowSet empty = rowset_new(1);
        return empty;
    }
    }
}

/* ===== Public entry point ===== */

int phys_exec_run(DiskDB *db, const PhysicalPlan *plan,
                  HugoResult *r, Arena *arena) {
    result_init(r);
    if (!plan) {
        strncpy(r->err_code, "NO_PLAN", sizeof(r->err_code) - 1);
        strncpy(r->err_msg,  "optimizer returned no plan", sizeof(r->err_msg) - 1);
        r->ok = 0;
        return -1;
    }

    RowSet rs = phys_exec_node(db, plan, arena);

    /* Transfer rows to HugoResult */
    r->ok    = 1;
    r->count = 0;
    for (int i = 0; i < rs.count && r->count < MAX_RESULT_DOCS; i++) {
        r->docs[r->count++] = rs.rows[i];
        rs.rows[i] = NULL; /* ownership transferred */
    }
    /* Free any excess rows that didn't fit */
    for (int i = r->count; i < rs.count; i++)
        if (rs.rows[i]) doc_free(rs.rows[i]);
    free(rs.rows);

    return 0;
}
