/* executor_disk.c â€” Executor using DiskDB
 *
 * Giá»‘ng executor.c nhÆ°ng: scan qua ddb_scan (load doc tá»« disk),
 * collect vÃ o HugoResult. ÄÃ¢y lÃ  clones (heap-allocated) â†’ caller pháº£i free.
 *
 * Insert/update/delete Ä‘i qua ddb_insert_doc / ddb_update_doc / ddb_delete_doc.
 */
#include "executor_disk.h"
#include "collection.h"
#include "optimizer/optimizer.h"
#include "phys_executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ===== Result helpers ===== */
static void set_err(HugoResult *r, const char *code, const char *msg) {
    r->ok = 0;
    strncpy(r->err_code, code, sizeof(r->err_code)-1);
    strncpy(r->err_msg, msg, sizeof(r->err_msg)-1);
}

static void set_info(HugoResult *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->info, sizeof(r->info), fmt, ap);
    va_end(ap);
}

/* ===== Condition evaluation (copy tá»« executor.c) ===== */
static int value_compare(const Value *a, const Value *b) {
    if (a->type != b->type) {
        if (a->type == VAL_NUM && b->type == VAL_NUM) return 0;
        return -2;
    }
    if (a->type == VAL_NUM) {
        if (a->num < b->num) return -1;
        if (a->num > b->num) return 1;
        return 0;
    }
    if (a->type == VAL_STR) return strcmp(a->str, b->str);
    return -2;
}

static int eval_cmp(const Condition *c, const Document *d) {
    Value dv;
    if (c->type == COND_EXISTS) {
        return doc_get_field(d, c->field, NULL) == 0;
    }
    if (doc_get_field(d, c->field, &dv) != 0) return 0;
    int cmp = value_compare(&dv, &c->value);
    switch (c->op) {
    case TOK_OP_BG:  return cmp == 0;
    case TOK_OP_KC:  return cmp != 0 && cmp != -2;
    case TOK_OP_LH:  return cmp == -1;
    case TOK_OP_BH:  return cmp == 1;
    case TOK_OP_LHB: return cmp == -1 || cmp == 0;
    case TOK_OP_BHB: return cmp == 1  || cmp == 0;
    case TOK_OP_XAU: {
        if (dv.type != VAL_STR || c->value.type != VAL_STR) return 0;
        return strstr(dv.str, c->value.str) != NULL;
    }
    default: return 0;
    }
}

static int eval_in(const Condition *c, const Document *d) {
    Value dv;
    if (doc_get_field(d, c->field, &dv) != 0) return 0;
    int found = 0;
    for (int i = 0; i < c->n_values; i++) {
        if (value_compare(&dv, &c->values[i]) == 0) { found = 1; break; }
    }
    return (c->op == TOK_OP_TG) ? found : !found;
}

static int eval_condition(const Condition *c, const Document *d) {
    if (!c) return 1;
    switch (c->type) {
    case COND_AND: return eval_condition(c->left, d) && eval_condition(c->right, d);
    case COND_OR:  return eval_condition(c->left, d) || eval_condition(c->right, d);
    case COND_NOT: return !eval_condition(c->left, d);
    case COND_IN:  return eval_in(c, d);
    case COND_CMP:
    case COND_EXISTS:
        return eval_cmp(c, d);
    }
    return 0;
}

/* ===== Sort ===== */
static const SortField *g_sort_head;
static int compare_docs(const void *a, const void *b) {
    const Document *da = *(const Document*const*)a;
    const Document *db = *(const Document*const*)b;
    for (const SortField *s = g_sort_head; s; s = s->next) {
        Value va, vb;
        int ha = doc_get_field(da, s->field, &va) == 0;
        int hb = doc_get_field(db, s->field, &vb) == 0;
        if (!ha && !hb) continue;
        if (!ha) return s->descending ? 1 : -1;
        if (!hb) return s->descending ? -1 : 1;
        int c = value_compare(&va, &vb);
        if (c == -2) continue;
        if (c != 0) return s->descending ? -c : c;
    }
    return 0;
}

/* ===== Scan context for funden ===== */
typedef struct {
    const Query *q;
    HugoResult  *r;
} ScanCtx;

static void funden_visit(uint64_t id, Document *d, void *ctx_) {
    (void)id;
    ScanCtx *sc = (ScanCtx*)ctx_;
    if (eval_condition(sc->q->haar, d)) {
        if (sc->r->count < MAX_RESULT_DOCS) {
            /* Clone â€” caller free */
            sc->r->docs[sc->r->count++] = doc_clone(d);
        }
    }
}

/* Collect ids for update/delete (can't modify during scan) */
typedef struct {
    const Query *q;
    uint64_t    *ids;
    int          n_ids;
    int          cap;
} CollectCtx;

static void collect_matching_visit(uint64_t id, Document *d, void *ctx_) {
    CollectCtx *cc = (CollectCtx*)ctx_;
    if (eval_condition(cc->q->haar, d)) {
        if (cc->n_ids >= cc->cap) {
            cc->cap = cc->cap ? cc->cap * 2 : 64;
            cc->ids = (uint64_t*)realloc(cc->ids, cc->cap * sizeof(uint64_t));
        }
        cc->ids[cc->n_ids++] = id;
    }
}

/* ===== Verb handlers ===== */
static void exec_funden(DiskDB *db, const Query *q, HugoResult *r) {
    DiskColl *c = ddb_get_coll(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }

    ScanCtx sc = { q, r };
    ddb_scan(db, c, funden_visit, &sc);

    if (q->orange_bi && r->count > 0) {
        g_sort_head = q->orange_bi;
        qsort(r->docs, r->count, sizeof(Document*), compare_docs);
    }

    if (q->skopan > 0) {
        int skip = q->skopan;
        if (skip >= r->count) {
            /* free skipped docs too */
            for (int i = 0; i < r->count; i++) doc_free(r->docs[i]);
            r->count = 0;
        } else {
            for (int i = 0; i < skip; i++) doc_free(r->docs[i]);
            int new_n = r->count - skip;
            memmove(r->docs, r->docs + skip, new_n * sizeof(Document*));
            r->count = new_n;
        }
    }
    if (q->lime >= 0 && r->count > q->lime) {
        for (int i = q->lime; i < r->count; i++) doc_free(r->docs[i]);
        r->count = q->lime;
    }
}

static void exec_vietinfo(DiskDB *db, const Query *q, HugoResult *r) {
    /* Batch insert */
    if (q->n_batch > 0) {
        DiskColl *c = ddb_get_coll(db, q->collection);
        if (!c) c = ddb_create_coll(db, q->collection);
        if (!c) { set_err(r, "MAX_COLLECTIONS", "too many collections"); return; }
        int inserted = 0;
        for (int i = 0; i < q->n_batch; i++) {
            Document *clone = doc_clone(q->batch_docs[i]);
            uint64_t id;
            int rc = ddb_insert_doc(db, c, clone, &id);
            doc_free(clone);
            if (rc == 0) inserted++;
        }
        r->count = inserted;
        set_info(r, "inserted %d documents", inserted);
        return;
    }
    if (!q->payload) { set_err(r, "NO_PAYLOAD", "vietinfo requires a document"); return; }
    DiskColl *c = ddb_get_coll(db, q->collection);
    if (!c) c = ddb_create_coll(db, q->collection);
    if (!c) { set_err(r, "MAX_COLLECTIONS", "too many collections"); return; }
    Document *clone = doc_clone(q->payload);
    uint64_t id;
    int rc = ddb_insert_doc(db, c, clone, &id);
    doc_free(clone);
    if (rc == -2) { set_err(r, "DOC_TOO_LARGE", "document too large for 1 page"); return; }
    if (rc != 0) { set_err(r, "IO", "insert failed"); return; }
    r->count = 1;
    set_info(r, "inserted id=%llu", (unsigned long long)id);
}

static void exec_cochin(DiskDB *db, const Query *q, HugoResult *r) {
    DiskColl *c = ddb_get_coll(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    if (!q->set_ops) { set_err(r, "NO_SET_OP", "cochin requires $quy/$don/$loi"); return; }

    CollectCtx cc = { q, NULL, 0, 0 };
    ddb_scan(db, c, collect_matching_visit, &cc);

    int affected = 0;
    for (int i = 0; i < cc.n_ids; i++) {
        Document *d = ddb_read_doc(db, c, cc.ids[i]);
        if (!d) continue;
        for (SetOp *op = q->set_ops; op; op = op->next) {
            if (op->op == TOK_OP_QUY) {
                doc_set_field(d, op->field, op->value);
            } else if (op->op == TOK_OP_DON) {
                Value existing; char new_str[256];
                const char *add_s = (op->value.type == VAL_STR) ? op->value.str : NULL;
                char nb[64]; if (!add_s) { snprintf(nb,sizeof(nb),"%g",op->value.num); add_s=nb; }
                if (doc_get_field(d, op->field, &existing)==0 && existing.type==VAL_STR && existing.str[0])
                    snprintf(new_str, sizeof(new_str), "%s,%s", existing.str, add_s);
                else snprintf(new_str, sizeof(new_str), "%s", add_s);
                Value v; memset(&v,0,sizeof(v)); v.type=VAL_STR;
                strncpy(v.str, new_str, sizeof(v.str)-1);
                doc_set_field(d, op->field, v);
            } else if (op->op == TOK_OP_LOI) {
                Value existing;
                if (doc_get_field(d, op->field, &existing)!=0 || existing.type!=VAL_STR) continue;
                const char *rm_s = (op->value.type==VAL_STR) ? op->value.str : NULL;
                char nb[64]; if (!rm_s) { snprintf(nb,sizeof(nb),"%g",op->value.num); rm_s=nb; }
                char res[256]={0}, tmp[256]; strncpy(tmp, existing.str, sizeof(tmp)-1);
                char *sv=NULL, *tk=strtok_s(tmp,",",&sv); int first=1;
                while(tk) { while(*tk==' ')tk++;
                    if(strcmp(tk,rm_s)!=0) { if(!first)strncat(res,",",sizeof(res)-strlen(res)-1); strncat(res,tk,sizeof(res)-strlen(res)-1); first=0; }
                    tk=strtok_s(NULL,",",&sv); }
                Value v; memset(&v,0,sizeof(v)); v.type=VAL_STR;
                strncpy(v.str, res, sizeof(v.str)-1);
                doc_set_field(d, op->field, v);
            }
        }
        ddb_update_doc(db, c, cc.ids[i], d);
        doc_free(d);
        affected++;
    }
    free(cc.ids);
    r->count = affected;
    if (affected == 0) set_err(r, "NOT_FOUND", "no document matches condition");
    else               set_info(r, "updated %d", affected);
}

static void exec_demlet(DiskDB *db, const Query *q, HugoResult *r) {
    DiskColl *c = ddb_get_coll(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }

    CollectCtx cc = { q, NULL, 0, 0 };
    ddb_scan(db, c, collect_matching_visit, &cc);

    for (int i = 0; i < cc.n_ids; i++) ddb_delete_doc(db, c, cc.ids[i]);
    r->count = cc.n_ids;
    set_info(r, "deleted %d", cc.n_ids);
    free(cc.ids);
}

static void exec_madeco(DiskDB *db, const Query *q, HugoResult *r) {
    if (ddb_get_coll(db, q->collection)) {
        set_err(r, "EXISTS", "collection already exists"); return;
    }
    if (!ddb_create_coll(db, q->collection)) {
        set_err(r, "MAX_COLLECTIONS", "too many collections"); return;
    }
    set_info(r, "created collection %s", q->collection);
}

static void exec_delco(DiskDB *db, const Query *q, HugoResult *r) {
    if (ddb_drop_coll(db, q->collection) != 0) {
        set_err(r, "NO_COLLECTION", "collection not found"); return;
    }
    set_info(r, "dropped collection %s", q->collection);
}

static void exec_skill(DiskDB *db, const Query *q, HugoResult *r) {
    if (q->collection[0] == 0) {
        char buf[1024] = {0};
        size_t off = snprintf(buf, sizeof(buf), "collections: ");
        for (int i = 0; i < db->n_colls; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%s",
                            i == 0 ? "" : ", ", db->colls[i].name);
        }
        strncpy(r->info, buf, sizeof(r->info)-1);
        r->count = db->n_colls;
    } else {
        DiskColl *c = ddb_get_coll(db, q->collection);
        if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
        r->count = (int)c->count;
        set_info(r, "collection %s: %llu documents",
                 q->collection, (unsigned long long)c->count);
    }
}

/* ===== Aggregation (gomail) ===== */
typedef struct AggGroup {
    Value   key;
    int     count;
    double  sums[16], mins[16], maxs[16];
    int     inited[16];
    struct AggGroup *next;
} AggGroup;

typedef struct { const Query *q; AggGroup **groups; int *n_groups; } AggScanCtx;

static void gomail_visit(uint64_t id, Document *d, void *ctx_) {
    (void)id;
    AggScanCtx *sc = (AggScanCtx*)ctx_;
    const Query *q = sc->q;
    if (!eval_condition(q->haar, d)) return;

    Value gk;
    if (doc_get_field(d, q->gremb_bi->field, &gk) != 0) gk.type = VAL_NULL;

    AggGroup *g = NULL;
    for (AggGroup *p = *sc->groups; p; p = p->next)
        if (value_compare(&p->key, &gk) == 0) { g = p; break; }
    if (!g) {
        g = (AggGroup*)calloc(1, sizeof(AggGroup));
        g->key = gk; g->next = *sc->groups; *sc->groups = g; (*sc->n_groups)++;
    }
    g->count++;
    for (int a = 0; a < q->gremb_bi->n_aggs; a++) {
        Value av;
        if (doc_get_field(d, q->gremb_bi->agg_fields[a], &av) != 0) continue;
        double v = (av.type == VAL_NUM) ? av.num : 0;
        g->sums[a] += v;
        if (!g->inited[a] || v < g->mins[a]) g->mins[a] = v;
        if (!g->inited[a] || v > g->maxs[a]) g->maxs[a] = v;
        g->inited[a] = 1;
    }
}

static void exec_gomail(DiskDB *db, const Query *q, HugoResult *r) {
    DiskColl *c = ddb_get_coll(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    if (!q->gremb_bi) { set_err(r, "NO_GROUP", "gomail requires gremb bi"); return; }

    AggGroup *groups = NULL; int n_groups = 0;
    AggScanCtx sc = { q, &groups, &n_groups };
    ddb_scan(db, c, gomail_visit, &sc);

    r->count = 0;
    for (AggGroup *g = groups; g && r->count < MAX_RESULT_DOCS; g = g->next) {
        Document *doc = (Document*)calloc(1, sizeof(Document));
        doc_set_field(doc, q->gremb_bi->field, g->key);
        for (int a = 0; a < q->gremb_bi->n_aggs; a++) {
            Value v; memset(&v, 0, sizeof(v)); v.type = VAL_NUM;
            char fname[256]; const char *an = "";
            switch (q->gremb_bi->agg_funcs[a]) {
            case TOK_POU: an="pou"; v.num=g->count; break;
            case TOK_SEP: an="sep"; v.num=g->sums[a]; break;
            case TOK_AWR: an="awr"; v.num=g->count>0?g->sums[a]/g->count:0; break;
            case TOK_MIE: an="mie"; v.num=g->mins[a]; break;
            case TOK_MAF: an="maf"; v.num=g->maxs[a]; break;
            default: break;
            }
            snprintf(fname, sizeof(fname), "%s_%s", an, q->gremb_bi->agg_fields[a]);
            doc_set_field(doc, fname, v);
        }
        r->docs[r->count++] = doc;
    }
    AggGroup *g = groups;
    while (g) { AggGroup *n = g->next; free(g); g = n; }
}

/* ===== Index operations ===== */
static void exec_madecoidu(DiskDB *db, const Query *q, HugoResult *r) {
    /* Parse "collection.field" from q->collection */
    char coll_name[64] = {0}, field_name[128] = {0};
    const char *dot = strchr(q->collection, '.');
    if (!dot) { set_err(r, "SYNTAX", "expected collection.field"); return; }
    size_t cn = (size_t)(dot - q->collection);
    if (cn >= sizeof(coll_name)) cn = sizeof(coll_name)-1;
    memcpy(coll_name, q->collection, cn);
    strncpy(field_name, dot+1, sizeof(field_name)-1);

    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    /* MVP: store index metadata only (no B-tree scan) */
    if (c->n_indexes >= DDB_MAX_INDEXES) { set_err(r, "MAX_INDEXES", "too many indexes"); return; }
    for (int i = 0; i < c->n_indexes; i++) {
        if (strcmp(c->indexes[i].field, field_name) == 0) {
            set_err(r, "EXISTS", "index already exists"); return;
        }
    }
    IndexMeta *im = &c->indexes[c->n_indexes++];
    strncpy(im->field, field_name, sizeof(im->field)-1);
    im->btree_root_page = 0; /* MVP: metadata-only index */
    db->dirty = 1;
    set_info(r, "created index %s.%s", coll_name, field_name);
}

static void exec_delecoidu(DiskDB *db, const Query *q, HugoResult *r) {
    char coll_name[64] = {0}, field_name[128] = {0};
    const char *dot = strchr(q->collection, '.');
    if (!dot) { set_err(r, "SYNTAX", "expected collection.field"); return; }
    size_t cn = (size_t)(dot - q->collection);
    if (cn >= sizeof(coll_name)) cn = sizeof(coll_name)-1;
    memcpy(coll_name, q->collection, cn);
    strncpy(field_name, dot+1, sizeof(field_name)-1);

    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    for (int i = 0; i < c->n_indexes; i++) {
        if (strcmp(c->indexes[i].field, field_name) == 0) {
            for (int j = i; j < c->n_indexes-1; j++) c->indexes[j] = c->indexes[j+1];
            c->n_indexes--;
            db->dirty = 1;
            set_info(r, "dropped index %s.%s", coll_name, field_name);
            return;
        }
    }
    set_err(r, "NOT_FOUND", "index not found");
}

static void exec_exepanus(DiskDB *db, const Query *q, HugoResult *r) {
    /* Explain: check if haar filter uses indexed field */
    const char *scan_type = "SCAN";
    const char *idx_field = NULL;
    if (q->haar && q->haar->type == COND_CMP) {
        /* Check if this field has index */
        DiskColl *c = ddb_get_coll(db, q->collection);
        if (c) {
            for (int i = 0; i < c->n_indexes; i++) {
                if (strcmp(c->indexes[i].field, q->haar->field) == 0) {
                    scan_type = "INDEX SCAN";
                    idx_field = c->indexes[i].field;
                    break;
                }
            }
        }
    }
    if (idx_field)
        set_info(r, "%s %s (field: %s)", scan_type, q->collection, idx_field);
    else
        set_info(r, "%s %s", scan_type, q->collection);
}

/* ===== Join ($rasoat) helper ===== */
static void apply_join(DiskDB *db, const Query *q, HugoResult *r) {
    if (!q->join) return;
    DiskColl *tc = ddb_get_coll(db, q->join->target_coll);
    if (!tc) return;
    for (int i = 0; i < r->count; i++) {
        if (!r->docs[i]) continue;
        Value lv;
        if (doc_get_field(r->docs[i], q->join->local_field, &lv) != 0) continue;
        /* Scan target collection for match */
        for (uint64_t tid = 1; tid < tc->capacity && tid < tc->next_id; tid++) {
            if (tc->doc_page_ids[tid] == 0) continue;
            Document *td_doc = ddb_read_doc(db, tc, tid);
            if (!td_doc) continue;
            Value tv;
            if (doc_get_field(td_doc, q->join->target_field, &tv) == 0 &&
                value_compare(&lv, &tv) == 0) {
                /* Embed target doc fields with alias prefix */
                for (KVPair *kv = td_doc->pairs; kv; kv = kv->next) {
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed), "%s.%s", q->join->alias, kv->key);
                    doc_set_field(r->docs[i], prefixed, kv->value);
                }
                doc_free(td_doc);
                break; /* first match only */
            }
            doc_free(td_doc);
        }
    }
}

int hugo_execute_disk(DiskDB *db, const Query *q, HugoResult *r) {
    result_init(r);
    if (q->error[0]) { set_err(r, "PARSE_ERROR", q->error); return 0; }

    switch (q->verb) {
    case VERB_FUNDEN:   exec_funden(db, q, r); if (r->ok) apply_join(db, q, r); break;
    case VERB_VIETINFO: exec_vietinfo(db, q, r); break;
    case VERB_COCHIN:   exec_cochin(db, q, r);   break;
    case VERB_DEMLET:   exec_demlet(db, q, r);   break;
    case VERB_MADECO:   exec_madeco(db, q, r);   break;
    case VERB_DELCO:    exec_delco(db, q, r);    break;
    case VERB_SKILL:    exec_skill(db, q, r);    break;
    case VERB_GOMAIL:   exec_gomail(db, q, r);   break;
    case VERB_MADECOIDU: exec_madecoidu(db, q, r); break;
    case VERB_DELECOIDU: exec_delecoidu(db, q, r); break;
    case VERB_EXEPANUS: exec_exepanus(db, q, r); break;
    case VERB_GINAN:
        if (db->wal_enabled) {
            db->in_tx = 1;
            db->current_tx_id = wal_new_tx_id(&db->wal);
            wal_log_begin(&db->wal, db->current_tx_id);
            wal_sync(&db->wal);
        }
        set_info(r, "transaction begin");
        break;
    case VERB_COMETI:
        if (db->in_tx && db->wal_enabled) {
            wal_log_commit(&db->wal, db->current_tx_id);
            wal_sync(&db->wal);
            db->in_tx = 0;
        }
        set_info(r, "transaction commit");
        break;
    case VERB_TULABERK:
        if (db->in_tx && db->wal_enabled) {
            wal_log_abort(&db->wal, db->current_tx_id);
            wal_sync(&db->wal);
            db->in_tx = 0;
        }
        set_info(r, "transaction abort");
        break;
    case VERB_USF:
        set_info(r, "using database %s", q->collection);
        break;
    default:
        set_err(r, "NOT_IMPLEMENTED", "verb deferred to later phase");
    }
    return 0;
}

/* ===== Print / free â€” docs lÃ  clones, cáº§n free ===== */
static void print_value(const Value *v) {
    switch (v->type) {
    case VAL_NUM:
        if (v->num == (double)(long long)v->num)
            printf("%lld", (long long)v->num);
        else
            printf("%g", v->num);
        break;
    case VAL_STR: printf("\"%s\"", v->str); break;
    case VAL_BOOL: printf("%s", v->num != 0 ? "true" : "false"); break;
    case VAL_NULL: default: printf("null");
    }
}

static void print_document(const Document *d) {
    printf("{ ");
    int first = 1;
    for (KVPair *kv = d->pairs; kv; kv = kv->next) {
        if (!first) printf(", ");
        printf("%s: ", kv->key);
        print_value(&kv->value);
        first = 0;
    }
    printf(" }");
}

void result_print_disk(const HugoResult *r) {
    if (!r->ok) {
        printf("err %s \"%s\"\n", r->err_code, r->err_msg);
        return;
    }
    if (r->count > 0 && r->docs[0]) {
        printf("ok %d document%s\n", r->count, r->count == 1 ? "" : "s");
        for (int i = 0; i < r->count; i++) {
            if (!r->docs[i]) continue;
            print_document(r->docs[i]);
            printf("\n");
        }
    } else {
        if (r->info[0]) printf("ok %s\n", r->info);
        else            printf("ok %d documents\n", r->count);
    }
}

void result_free_disk(HugoResult *r) {
    for (int i = 0; i < r->count; i++) {
        if (r->docs[i]) doc_free(r->docs[i]);
        r->docs[i] = NULL;
    }
    r->count = 0;
}

/* ===== Optimizer-aware execute entry point (Stage 3) ===== */

/* Global optimizer context â€” initialized once per DiskDB open.
 * In a multi-connection server this would be per-connection; for Hugo DB
 * single-process model, global is sufficient. */
static OptimizerCtx g_opt_ctx;
static int          g_opt_ctx_ready = 0;

void hugo_optimizer_init(DiskDB *db, const char *db_path, OptimizerMode mode) {
    optimizer_ctx_init(&g_opt_ctx, db, db_path);
    optimizer_set_mode(&g_opt_ctx, mode);
    g_opt_ctx_ready = 1;
}

void hugo_optimizer_set_trace(int trace) {
    if (g_opt_ctx_ready) optimizer_set_trace(&g_opt_ctx, trace);
}

OptimizerCtx* hugo_optimizer_get(void) {
    return g_opt_ctx_ready ? &g_opt_ctx : NULL;
}

/* Execute with optimizer:
 *   - VERB_ANALYZE: rebuild stats for collection
 *   - VERB_EXEPANUS: use optimizer EXPLAIN (rich output)
 *   - VERB_FUNDEN / VERB_GOMAIL: route through optimizer if mode != OFF
 *   - All other verbs: fall through to legacy executor
 */
int hugo_execute_disk_opt(DiskDB *db, const Query *q, HugoResult *r) {
    result_init(r);
    if (q->error[0]) {
        strncpy(r->err_code, "PARSE_ERROR", sizeof(r->err_code)-1);
        strncpy(r->err_msg,  q->error, sizeof(r->err_msg)-1);
        r->ok = 0;
        return 0;
    }

    /* VERB_ANALYZE: always handled here regardless of optimizer mode */
    if (q->verb == VERB_ANALYZE) {
        if (!g_opt_ctx_ready) {
            strncpy(r->err_code, "OPT_NOT_INIT", sizeof(r->err_code)-1);
            strncpy(r->err_msg, "optimizer not initialized", sizeof(r->err_msg)-1);
            r->ok = 0;
            return 0;
        }
        int rc = optimizer_analyze(&g_opt_ctx, q->collection);
        if (rc != 0) {
            snprintf(r->err_msg, sizeof(r->err_msg),
                     "analyze failed for collection '%s'", q->collection);
            strncpy(r->err_code, "ANALYZE_ERR", sizeof(r->err_code)-1);
            r->ok = 0;
        } else {
            snprintf(r->info, sizeof(r->info),
                     "analyzed collection '%s' â€” statistics updated", q->collection);
            r->ok    = 1;
            r->count = 0;
        }
        return 0;
    }

    /* If optimizer not ready or mode is OFF â†’ legacy path */
    if (!g_opt_ctx_ready || g_opt_ctx.mode == HUGO_OPT_OFF) {
        return hugo_execute_disk(db, q, r);
    }

    /* VERB_EXEPANUS (EXPLAIN) with optimizer */
    if (q->verb == VERB_EXEPANUS) {
        Arena *arena = arena_new();
        if (!arena) { hugo_execute_disk(db, q, r); return 0; }

        PhysicalPlan *plan = optimizer_run(&g_opt_ctx, q, arena);
        if (!plan) {
            arena_free(arena);
            hugo_execute_disk(db, q, r);
            return 0;
        }
        char explain_buf[4096];
        optimizer_explain(plan, explain_buf, sizeof(explain_buf));
        strncpy(r->info, explain_buf, sizeof(r->info) - 1);
        r->ok    = 1;
        r->count = 0;
        arena_free(arena);
        return 0;
    }

    /* READ queries (FUNDEN, GOMAIL) â†’ optimizer path */
    if (q->verb == VERB_FUNDEN || q->verb == VERB_GOMAIL) {
        Arena *arena = arena_new();
        if (!arena) return hugo_execute_disk(db, q, r);

        PhysicalPlan *plan = optimizer_run(&g_opt_ctx, q, arena);
        if (!plan) {
            arena_free(arena);
            return hugo_execute_disk(db, q, r);  /* fallback */
        }

        int rc = phys_exec_run(db, plan, r, arena);
        arena_free(arena);
        return rc;
    }

    /* All other verbs (writes, DDL, transactions) â†’ legacy path */
    return hugo_execute_disk(db, q, r);
}

