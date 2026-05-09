/* vec_exec.c — Vectorized execution pipeline
 *
 * Luồng xử lý:
 *   1. Walk PhysicalPlan tree để thu thập tất cả fields cần thiết
 *   2. ddb_scan → col_batch_add_doc() mỗi row (1 lần duy nhất)
 *   3. vec_filter_apply() trên ColBatch
 *   4. Nếu có GROUP BY → vec_agg_run() → materialize ra Document*
 *   5. Nếu có ORDER BY → vec_sort_full() hoặc vec_sort_topk()
 *   6. vec_sort_apply_limit() → slice perm[]
 *   7. Rebuild Document* chỉ với rows còn lại → HugoResult
 *
 * JOIN: vẫn dùng hash join từ phys_executor (ColBatch chưa hỗ trợ 2 tables).
 * Sau join, kết quả được load vào ColBatch tiếp tục pipeline.
 */
#include "vec_exec.h"
#include "col_batch.h"
#include "vec_filter.h"
#include "vec_agg.h"
#include "vec_str_intern.h"
#include "vec_sort.h"
#include "../core/collection.h"
#include "vec_scan_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== Global scan cache ===== */
static ScanCache g_scan_cache;
static int       g_scan_cache_ready = 0;

static ScanCache* get_cache(void) {
    if (!g_scan_cache_ready) {
        scan_cache_init(&g_scan_cache);
        g_scan_cache_ready = 1;
    }
    return &g_scan_cache;
}

/* ===== Field collector ===== */
/* Walk plan tree, thu thập tất cả field names + types cần thiết cho ColBatch */

#define FC_MAX 64

typedef struct {
    char    fields[FC_MAX][128];
    ColType types [FC_MAX];
    int     n;
} FieldCollector;

static void fc_add(FieldCollector *fc, const char *field, ColType type) {
    if (!field || !field[0] || fc->n >= FC_MAX) return;
    /* Deduplicate */
    for (int i = 0; i < fc->n; i++)
        if (strcmp(fc->fields[i], field) == 0) return;
    strncpy(fc->fields[fc->n], field, 127);
    fc->types[fc->n] = type;
    fc->n++;
}

/* Heuristic: nếu không biết type của field, đăng ký cả 2 — num trước.
 * vec_filter sẽ dùng type nào match với Condition. */
static void fc_add_condition(FieldCollector *fc, const Condition *c) {
    if (!c) return;
    switch (c->type) {
    case COND_CMP:
    case COND_EXISTS:
    case COND_IN: {
        /* Infer type từ value */
        ColType t = (c->value.type == VAL_STR) ? COL_TYPE_STR : COL_TYPE_NUM;
        /* EXISTS: không có value, đăng ký num (nếu field là str thì sẽ miss, ok) */
        if (c->type == COND_EXISTS) t = COL_TYPE_NUM;
        fc_add(fc, c->field, t);
        /* Nếu IN với string values thì cũng đăng ký STR */
        if (c->type == COND_IN && c->n_values > 0 && c->values[0].type == VAL_STR)
            fc_add(fc, c->field, COL_TYPE_STR);
        break;
    }
    case COND_AND:
    case COND_OR:
    case COND_NOT:
        fc_add_condition(fc, c->left);
        fc_add_condition(fc, c->right);
        break;
    }
}

static void fc_walk_plan(FieldCollector *fc, const PhysicalPlan *plan) {
    if (!plan) return;
    switch (plan->type) {
    case POP_FILTER:
        fc_add_condition(fc, plan->filter.predicate);
        break;
    case POP_SORT:
        for (const SortField *s = plan->sort.fields; s; s = s->next)
            fc_add(fc, s->field, COL_TYPE_NUM); /* try num first — sort prefers num */
        break;
    case POP_HASH_AGGREGATE:
    case POP_STREAM_AGGREGATE:
        fc_add(fc, plan->aggregate.group_by_field, COL_TYPE_STR); /* group key as str */
        fc_add(fc, plan->aggregate.group_by_field, COL_TYPE_NUM); /* also try num */
        for (size_t a = 0; a < plan->aggregate.n_aggs; a++)
            fc_add(fc, plan->aggregate.aggs[a].field, COL_TYPE_NUM);
        break;
    case POP_PROJECT:
        for (size_t i = 0; i < plan->project.n_columns; i++)
            if (plan->project.columns[i])
                fc_add(fc, plan->project.columns[i], COL_TYPE_NUM);
        break;
    default:
        break;
    }
    fc_walk_plan(fc, plan->left);
    fc_walk_plan(fc, plan->right);
}

/* Deduplicate fields: nếu cùng field đăng ký cả NUM lẫn STR,
 * giữ STR và xóa NUM (STR col_batch sẽ trả NULL cho non-string rows).
 * Thực ra với design hiện tại ColBatch mỗi field name chỉ 1 entry — ok. */

/* ===== Scan context ===== */
typedef struct {
    ColBatch *batch;
    int       row;
    int       cap;
} ScanCtx;

static void scan_visit(uint64_t id, Document *doc, void *ctx_) {
    (void)id;
    ScanCtx *sc = (ScanCtx*)ctx_;
    if (sc->row >= sc->cap) return;
    /* doc được ddb_scan cấp và sẽ doc_free() ngay sau callback.
     * Chúng ta cần clone để giữ cho suốt query pipeline. */
    Document *owned = doc_clone(doc);
    if (!owned) return;
    col_batch_add_doc(sc->batch, owned, sc->row);
    sc->row++;
}

/* ===== Find leaf scan node ===== */
static const PhysicalPlan* find_scan(const PhysicalPlan *plan) {
    if (!plan) return NULL;
    if (plan->type == POP_SEQ_SCAN || plan->type == POP_INDEX_SCAN)
        return plan;
    const PhysicalPlan *l = find_scan(plan->left);
    if (l) return l;
    return find_scan(plan->right);
}

/* ===== Find nodes by type in plan tree ===== */
static const PhysicalPlan* find_node(const PhysicalPlan *plan, PhysicalOpType t) {
    if (!plan) return NULL;
    if (plan->type == t) return plan;
    const PhysicalPlan *l = find_node(plan->left, t);
    if (l) return l;
    return find_node(plan->right, t);
}

/* ===== Hash join (fallback, ported from phys_executor) ===== */
#define HJ_BUCKETS 4096

typedef struct HJEntry { Document *doc; struct HJEntry *next; } HJEntry;
typedef struct { HJEntry *buckets[HJ_BUCKETS]; } HashTable;

static uint64_t hj_hash(const Value *v) {
    if (v->type == VAL_NUM) {
        uint64_t b; memcpy(&b, &v->num, 8);
        return b ^ (b >> 17) ^ (b >> 31);
    }
    uint64_t h = 14695981039346656037ULL;
    for (const char *p = v->str; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    return h;
}

static int val_eq(const Value *a, const Value *b) {
    if (a->type != b->type) return 0;
    if (a->type == VAL_NUM) return a->num == b->num;
    if (a->type == VAL_STR) return strcmp(a->str, b->str) == 0;
    return 0;
}

/* Scan một collection vào Document** array, trả về count */
static int scan_to_array(DiskDB *db, const char *coll_name,
                          Document ***out, Arena *arena) {
    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) { *out = NULL; return 0; }

    /* Estimate capacity từ c->count */
    int cap = (int)(c->count > 0 ? c->count : 256);
    Document **arr = (Document**)arena_alloc(arena, cap * sizeof(Document*));
    if (!arr) { *out = NULL; return 0; }

    int n = 0;
    for (uint64_t id = 1; id < c->capacity && id < c->next_id; id++) {
        if (c->doc_page_ids[id] == 0) continue;
        Document *d = ddb_read_doc(db, c, id);
        if (!d) continue;
        if (n >= cap) {
            /* Grow (arena can't realloc, so alloc new bigger block) */
            Document **narr = (Document**)arena_alloc(arena, cap*2*sizeof(Document*));
            if (!narr) { doc_free(d); break; }
            memcpy(narr, arr, n * sizeof(Document*));
            arr = narr; cap *= 2;
        }
        arr[n++] = d; /* owned */
    }
    *out = arr;
    return n;
}

static void do_hash_join(DiskDB *db, const PhysicalPlan *plan,
                          Document ***left_out, int *left_n,
                          Arena *arena) {
    /* Build side = right (smaller), probe = left */
    Document **right = NULL; int right_n = 0;
    Document **left  = NULL; int ln = 0;

    const PhysicalPlan *lp = plan->left;
    const PhysicalPlan *rp = plan->right;

    /* Get collection names from leaf scans */
    const PhysicalPlan *ls = find_scan(lp);
    const PhysicalPlan *rs = find_scan(rp);
    if (!ls || !rs) { *left_out = NULL; *left_n = 0; return; }

    const char *lcoll = (ls->type == POP_SEQ_SCAN)
                        ? ls->seq_scan.collection_name
                        : ls->index_scan.collection_name;
    const char *rcoll = (rs->type == POP_SEQ_SCAN)
                        ? rs->seq_scan.collection_name
                        : rs->index_scan.collection_name;

    ln      = scan_to_array(db, lcoll, &left,  arena);
    right_n = scan_to_array(db, rcoll, &right, arena);

    /* Build hash table on right.right_col */
    HashTable *ht = (HashTable*)arena_alloc(arena, sizeof(HashTable));
    memset(ht, 0, sizeof(HashTable));
    for (int i = 0; i < right_n; i++) {
        Value kv; if (doc_get_field(right[i], plan->join.right_col, &kv) != 0) continue;
        int h = (int)(hj_hash(&kv) % HJ_BUCKETS);
        HJEntry *e = (HJEntry*)arena_alloc(arena, sizeof(HJEntry));
        e->doc = right[i]; e->next = ht->buckets[h]; ht->buckets[h] = e;
    }

    /* Probe */
    int out_cap = ln > 0 ? ln : 16;
    Document **out = (Document**)arena_alloc(arena, out_cap * sizeof(Document*));
    int out_n = 0;
    for (int i = 0; i < ln; i++) {
        Value pv; if (doc_get_field(left[i], plan->join.left_col, &pv) != 0) continue;
        int h = (int)(hj_hash(&pv) % HJ_BUCKETS);
        for (HJEntry *e = ht->buckets[h]; e; e = e->next) {
            Value bv; if (doc_get_field(e->doc, plan->join.right_col, &bv) != 0) continue;
            if (!val_eq(&pv, &bv)) continue;
            Document *merged = doc_clone(left[i]);
            for (KVPair *kv = e->doc->pairs; kv; kv = kv->next)
                doc_set_field(merged, kv->key, kv->value);
            if (out_n >= out_cap) {
                Document **no = (Document**)arena_alloc(arena, out_cap*2*sizeof(Document*));
                if (!no) break;
                memcpy(no, out, out_n*sizeof(Document*)); out = no; out_cap *= 2;
            }
            out[out_n++] = merged;
            break;
        }
    }
    /* Free left/right (not in arena) */
    for (int i = 0; i < ln;      i++) if (left[i])  doc_free(left[i]);
    for (int i = 0; i < right_n; i++) if (right[i]) doc_free(right[i]);

    *left_out = out; *left_n = out_n;
}

/* ===== Build ColBatch từ Document** array (sau join) ===== */
static ColBatch* batch_from_array(Document **docs, int n,
                                   FieldCollector *fc, Arena *arena) {
    if (n == 0 || fc->n == 0) return NULL;
    const char *fnames[FC_MAX];
    for (int i = 0; i < fc->n; i++) fnames[i] = fc->fields[i];

    ColBatch *b = col_batch_new(arena, fnames, fc->types, fc->n, n);
    if (!b) return NULL;
    for (int i = 0; i < n; i++) col_batch_add_doc(b, docs[i], i);
    col_batch_finalize(b, n);
    return b;
}

/* ===== Materialize alive perm rows into HugoResult ===== */
static void materialize_result(const ColBatch *b, int *perm, int perm_n,
                                HugoResult *r) {
    r->count = 0;
    for (int i = 0; i < perm_n && r->count < MAX_RESULT_DOCS; i++) {
        int ri = perm ? perm[i] : i;
        Document *d = b->docs[ri];
        if (!d) continue;
        r->docs[r->count++] = doc_clone(d);
    }
    r->ok = 1;
}

/* ===== Main entry ===== */

ScanCache* vec_get_scan_cache(void) { return get_cache(); }

int vec_exec_run(DiskDB *db, const PhysicalPlan *plan,
                 HugoResult *r, Arena *arena)
{
    result_init(r);
    if (!plan) {
        strncpy(r->err_code, "NO_PLAN", sizeof(r->err_code)-1);
        strncpy(r->err_msg,  "no physical plan", sizeof(r->err_msg)-1);
        r->ok = 0; return -1;
    }

    /* ── Step 1: collect all fields needed ── */
    FieldCollector fc; memset(&fc, 0, sizeof(fc));
    fc_walk_plan(&fc, plan);

    /* ── Step 2: get documents ── */
    Document **raw_docs = NULL;
    int        raw_n    = 0;
    int        has_join = 0;

    /* Check for join */
    const PhysicalPlan *join_node =
        find_node(plan, POP_HASH_JOIN);
    if (!join_node) join_node = find_node(plan, POP_NESTED_LOOP_JOIN);
    if (!join_node) join_node = find_node(plan, POP_SORT_MERGE_JOIN);

    if (join_node) {
        has_join = 1;
        do_hash_join(db, join_node, &raw_docs, &raw_n, arena);
    } else {
        /* Simple scan */
        const PhysicalPlan *scan = find_scan(plan);
        if (!scan) {
            strncpy(r->err_code, "NO_SCAN", sizeof(r->err_code)-1);
            strncpy(r->err_msg,  "no scan node in plan", sizeof(r->err_msg)-1);
            r->ok = 0; return -1;
        }
        const char *coll_name = (scan->type == POP_SEQ_SCAN)
                                ? scan->seq_scan.collection_name
                                : scan->index_scan.collection_name;
        DiskColl *coll = ddb_get_coll(db, coll_name);
        if (!coll) {
            r->ok = 1; r->count = 0; return 0; /* empty collection = empty result */
        }

        int cap = (int)(coll->count > 0 ? coll->count : 64);
        raw_docs = (Document**)arena_alloc(arena, cap * sizeof(Document*));
        if (!raw_docs) { r->ok=0; return -1; }

        /* Build ColBatch trực tiếp trong scan callback */
        const char *fnames[FC_MAX];
        for (int i = 0; i < fc.n; i++) fnames[i] = fc.fields[i];

        ColBatch *b = (fc.n > 0)
                      ? col_batch_new(arena, fnames, fc.types, fc.n, cap)
                      : NULL;

        /* Scan: use cache to avoid repeated disk reads.
         * Cached docs are BORROWED (cache owns them) — do NOT free raw_docs
         * when using cache path. Set owned_raw = 0. */
        int owned_raw = 0;
        Document **cached = NULL;
        int cache_n = scan_cache_get(get_cache(), db, coll_name, &cached);

        if (cache_n >= 0 && cached) {
            /* Cache hit: borrow pointers, no copy needed */
            raw_docs  = cached;
            raw_n     = cache_n;
            owned_raw = 0; /* borrowed from cache */
            if (b) {
                /* Rebuild ColBatch if cap was based on stale count */
                if (raw_n > cap) {
                    const char *fn2[FC_MAX];
                    for (int i=0;i<fc.n;i++) fn2[i]=fc.fields[i];
                    b = col_batch_new(arena, fn2, fc.types, fc.n, raw_n);
                }
                for (int i = 0; i < raw_n; i++)
                    col_batch_add_doc(b, cached[i], i);
            }
        } else {
            /* Cache miss fallback: read from disk */
            owned_raw = 1;
            raw_n = 0;
            for (uint64_t id = 1; id < coll->capacity && id < coll->next_id; id++) {
                if (coll->doc_page_ids[id] == 0) continue;
                Document *d = ddb_read_doc(db, coll, id);
                if (!d) continue;
                if (raw_n >= cap) {
                    Document **nd = (Document**)arena_alloc(arena, cap*2*sizeof(Document*));
                    if (!nd) { doc_free(d); break; }
                    memcpy(nd, raw_docs, raw_n*sizeof(Document*));
                    raw_docs = nd; cap *= 2;
                }
                raw_docs[raw_n] = d;
                if (b) col_batch_add_doc(b, d, raw_n);
                raw_n++;
            }
        }
        (void)owned_raw; /* used below for free logic */

        if (b) {
            col_batch_finalize(b, raw_n);

            /* ── Step 3: filter ── */
            const PhysicalPlan *filter_node = find_node(plan, POP_FILTER);
            if (filter_node && filter_node->filter.predicate)
                vec_filter_apply(b, filter_node->filter.predicate);

            /* ── Step 4: aggregate ── */
            const PhysicalPlan *agg_node =
                find_node(plan, POP_HASH_AGGREGATE);
            if (!agg_node) agg_node = find_node(plan, POP_STREAM_AGGREGATE);

            if (agg_node && agg_node->aggregate.group_by_field[0]) {
                VecAggTable *tbl = vec_agg_new(arena, b,
                    agg_node->aggregate.group_by_field,
                    agg_node->aggregate.aggs,
                    (int)agg_node->aggregate.n_aggs);
                if (tbl) {
                    vec_agg_run_fast(tbl, b, arena);
                    Document *agg_docs[VEC_AGG_MAX_GROUPS];
                    int ag_n = vec_agg_materialize(tbl, agg_docs,
                                                   VEC_AGG_MAX_GROUPS);
                    /* Transfer to result */
                    r->count = 0; r->ok = 1;
                    for (int i = 0; i < ag_n && r->count < MAX_RESULT_DOCS; i++)
                        r->docs[r->count++] = agg_docs[i];
                    /* Free raw docs only if we own them */
                    if (owned_raw)
                        for (int i = 0; i < raw_n; i++)
                            if (raw_docs[i]) doc_free(raw_docs[i]);
                    return 0;
                }
            }

            /* ── Step 5: sort ── */
            const PhysicalPlan *sort_node = find_node(plan, POP_SORT);
            const PhysicalPlan *limit_node = find_node(plan, POP_LIMIT);
            int lim  = limit_node ? limit_node->limit.limit : -1;
            int skip = limit_node ? limit_node->limit.skip  :  0;

            if (sort_node && sort_node->sort.fields) {
                /* Use topk if limit is small vs n_rows */
                int alive_n = col_batch_alive_count(b);
                int effective_k = (skip >= 0 && lim >= 0) ? skip + lim : alive_n;
                if (lim >= 0 && effective_k < alive_n / 8)
                    vec_sort_topk(b, sort_node->sort.fields, effective_k, arena);
                else
                    vec_sort_full(b, sort_node->sort.fields, arena);
            }

            /* ── Step 6: apply limit ── */
            int out_n;
            int *out_perm = NULL;

            if (b->perm) {
                int alive_n = col_batch_alive_count(b);
                if (sort_node) {
                    /* perm already built by sort */
                    out_n = vec_sort_apply_limit(b, skip, lim);
                } else {
                    /* Build perm manually for alive rows */
                    b->perm = (int32_t*)arena_alloc(arena, raw_n * sizeof(int32_t));
                    out_n = 0;
                    for (int i = 0; i < b->n_rows && out_n < alive_n; i++)
                        if (b->alive[i]) b->perm[out_n++] = i;
                    out_n = vec_sort_apply_limit(b, skip, lim);
                }
                out_perm = (int*)b->perm;
            } else {
                /* No sort: linear scan of alive rows with skip/limit */
                b->perm = (int32_t*)arena_alloc(arena, raw_n * sizeof(int32_t));
                if (b->perm) {
                    int w = 0;
                    for (int i = 0; i < b->n_rows; i++)
                        if (b->alive[i]) b->perm[w++] = i;
                    out_n = vec_sort_apply_limit(b, skip, lim);
                    out_perm = (int*)b->perm;
                } else {
                    out_n = b->n_rows; /* fallback */
                }
            }

            /* ── Step 7: materialize ── */
            r->count = 0; r->ok = 1;
            for (int i = 0; i < out_n && r->count < MAX_RESULT_DOCS; i++) {
                int ri = out_perm ? out_perm[i] : i;
                if (ri >= b->n_rows || !b->docs[ri]) continue;
                r->docs[r->count++] = doc_clone(b->docs[ri]);
            }

            /* Free raw docs only if we own them (not cached) */
            if (owned_raw)
                for (int i = 0; i < raw_n; i++)
                    if (raw_docs[i]) doc_free(raw_docs[i]);
            return 0;
        }

        /* fc.n == 0: no fields needed (e.g. COUNT(*) with no group by).
         * Fall through to simple materialize. */
        {
        const PhysicalPlan *lp0 = find_node(plan, POP_LIMIT);
        int lim0  = lp0 ? lp0->limit.limit : -1;
        int skip0 = lp0 ? lp0->limit.skip  :  0;
        r->count = 0; r->ok = 1;
        int start = (skip0 > 0) ? skip0 : 0;
        for (int i = start; i < raw_n && r->count < MAX_RESULT_DOCS; i++) {
            if (lim0 >= 0 && r->count >= lim0) break;
            r->docs[r->count++] = doc_clone(raw_docs[i]);
        }
        }
        if (owned_raw)
            for (int i = 0; i < raw_n; i++) if (raw_docs[i]) doc_free(raw_docs[i]);
        return 0;
    }

    /* ── Join path: build ColBatch từ joined docs ── */
    if (has_join && raw_docs && raw_n > 0) {
        ColBatch *b = batch_from_array(raw_docs, raw_n, &fc, arena);

        if (b) {
            const PhysicalPlan *filter_node = find_node(plan, POP_FILTER);
            if (filter_node && filter_node->filter.predicate)
                vec_filter_apply(b, filter_node->filter.predicate);

            const PhysicalPlan *sort_node  = find_node(plan, POP_SORT);
            const PhysicalPlan *limit_node = find_node(plan, POP_LIMIT);
            int lim  = limit_node ? limit_node->limit.limit : -1;
            int skip = limit_node ? limit_node->limit.skip  :  0;

            if (sort_node && sort_node->sort.fields)
                vec_sort_full(b, sort_node->sort.fields, arena);

            b->perm = (int32_t*)arena_alloc(arena, raw_n * sizeof(int32_t));
            int w = 0;
            for (int i = 0; i < b->n_rows; i++)
                if (b->alive[i]) b->perm[w++] = i;
            int out_n = vec_sort_apply_limit(b, skip, lim);

            r->count = 0; r->ok = 1;
            for (int i = 0; i < out_n && r->count < MAX_RESULT_DOCS; i++) {
                int ri = b->perm[i];
                if (b->docs[ri]) r->docs[r->count++] = doc_clone(b->docs[ri]);
            }
        } else {
            /* Fallback: just materialize raw_docs */
            r->count = 0; r->ok = 1;
            int lim2 = -1; int start = 0;
            const PhysicalPlan *lp = find_node(plan, POP_LIMIT);
            if (lp) { start = lp->limit.skip; lim2 = lp->limit.limit; }
            for (int i = start; i < raw_n && r->count < MAX_RESULT_DOCS; i++) {
                if (lim2 >= 0 && r->count >= lim2) break;
                r->docs[r->count++] = doc_clone(raw_docs[i]);
            }
        }

        for (int i = 0; i < raw_n; i++) if (raw_docs[i]) doc_free(raw_docs[i]);
    }

    return 0;
}
