/* executor.c — HugoQL executor */
#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ===== Result helpers ===== */
void result_init(HugoResult *r) {
    memset(r, 0, sizeof(*r));
    r->ok = 1;
}

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

/* ===== Value comparison ===== */
static int value_compare(const Value *a, const Value *b) {
    /* -1 if a<b, 0 if equal, 1 if a>b, -2 if incomparable */
    if (a->type != b->type) {
        /* allow NUM vs STR numeric compare? MVP: incomparable */
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

static int contains_substring(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

/* ===== Condition evaluation ===== */
static int eval_cmp(const Condition *c, const Document *d) {
    Value dv;
    if (c->type == COND_EXISTS) {
        return doc_get_field(d, c->field, NULL) == 0;
    }
    if (doc_get_field(d, c->field, &dv) != 0) return 0;  /* field missing */

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
        return contains_substring(dv.str, c->value.str);
    }
    default: return 0;
    }
}

/* ===== IN-list evaluation ($tg / $ktg) ===== */
static int eval_in(const Condition *c, const Document *d) {
    Value dv;
    if (doc_get_field(d, c->field, &dv) != 0) return 0;
    int found = 0;
    for (int i = 0; i < c->n_values; i++) {
        if (value_compare(&dv, &c->values[i]) == 0) {
            found = 1;
            break;
        }
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
    const Document *da = *(const Document**)a;
    const Document *db = *(const Document**)b;
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

/* ===== Verb handlers ===== */
static void exec_funden(HugoDatabase *db, const Query *q, HugoResult *r) {
    Collection *c = db_get_collection(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }

    /* Scan + filter */
    for (int i = 0; i < c->capacity; i++) {
        if (!c->docs[i]) continue;
        if (eval_condition(q->haar, c->docs[i])) {
            if (r->count < MAX_RESULT_DOCS)
                r->docs[r->count++] = c->docs[i];
        }
    }

    /* Sort */
    if (q->orange_bi && r->count > 0) {
        g_sort_head = q->orange_bi;
        qsort(r->docs, r->count, sizeof(Document*), compare_docs);
    }

    /* Skip + Limit */
    if (q->skopan > 0) {
        int skip = q->skopan;
        if (skip >= r->count) { r->count = 0; }
        else {
            int new_n = r->count - skip;
            memmove(r->docs, r->docs + skip, new_n * sizeof(Document*));
            r->count = new_n;
        }
    }
    if (q->lime >= 0 && r->count > q->lime) r->count = q->lime;
}

static void exec_vietinfo(HugoDatabase *db, const Query *q, HugoResult *r) {
    /* Batch insert */
    if (q->n_batch > 0) {
        Collection *c = db_get_collection(db, q->collection);
        if (!c) c = db_create_collection(db, q->collection);
        if (!c) { set_err(r, "MAX_COLLECTIONS", "too many collections"); return; }
        int inserted = 0;
        for (int i = 0; i < q->n_batch; i++) {
            Document *clone = doc_clone(q->batch_docs[i]);
            coll_insert(c, clone);
            inserted++;
        }
        db->dirty = 1;
        r->count = inserted;
        set_info(r, "inserted %d documents", inserted);
        return;
    }
    /* Single insert */
    if (!q->payload) {
        set_err(r, "NO_PAYLOAD", "vietinfo requires a document");
        return;
    }
    Collection *c = db_get_collection(db, q->collection);
    if (!c) c = db_create_collection(db, q->collection);
    if (!c) { set_err(r, "MAX_COLLECTIONS", "too many collections"); return; }

    Document *clone = doc_clone(q->payload);
    uint64_t id = coll_insert(c, clone);
    db->dirty = 1;
    r->count = 1;
    set_info(r, "inserted id=%llu", (unsigned long long)id);
}

static void exec_cochin(HugoDatabase *db, const Query *q, HugoResult *r) {
    Collection *c = db_get_collection(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    if (!q->set_ops) { set_err(r, "NO_SET_OP", "cochin requires $quy/$don/$loi"); return; }

    int affected = 0;
    for (int i = 0; i < c->capacity; i++) {
        if (!c->docs[i]) continue;
        if (!eval_condition(q->haar, c->docs[i])) continue;

        for (SetOp *op = q->set_ops; op; op = op->next) {
            if (op->op == TOK_OP_QUY) {
                doc_set_field(c->docs[i], op->field, op->value);
            } else if (op->op == TOK_OP_DON) {
                /* $don: append value to comma-separated string */
                Value existing;
                char new_str[256];
                const char *add_str = (op->value.type == VAL_STR) ? op->value.str : NULL;
                char num_buf[64];
                if (!add_str) {
                    snprintf(num_buf, sizeof(num_buf), "%g", op->value.num);
                    add_str = num_buf;
                }
                if (doc_get_field(c->docs[i], op->field, &existing) == 0 && existing.type == VAL_STR && existing.str[0]) {
                    snprintf(new_str, sizeof(new_str), "%s,%s", existing.str, add_str);
                } else {
                    snprintf(new_str, sizeof(new_str), "%s", add_str);
                }
                Value v; memset(&v, 0, sizeof(v));
                v.type = VAL_STR;
                strncpy(v.str, new_str, sizeof(v.str)-1);
                doc_set_field(c->docs[i], op->field, v);
            } else if (op->op == TOK_OP_LOI) {
                /* $loi: remove value from comma-separated string */
                Value existing;
                if (doc_get_field(c->docs[i], op->field, &existing) != 0 || existing.type != VAL_STR) continue;
                const char *rm_str = (op->value.type == VAL_STR) ? op->value.str : NULL;
                char num_buf[64];
                if (!rm_str) {
                    snprintf(num_buf, sizeof(num_buf), "%g", op->value.num);
                    rm_str = num_buf;
                }
                char result[256] = {0};
                char temp[256];
                strncpy(temp, existing.str, sizeof(temp)-1);
                char *saveptr = NULL;
                char *tok = strtok_s(temp, ",", &saveptr);
                int first = 1;
                while (tok) {
                    /* Trim spaces */
                    while (*tok == ' ') tok++;
                    if (strcmp(tok, rm_str) != 0) {
                        if (!first) strncat(result, ",", sizeof(result)-strlen(result)-1);
                        strncat(result, tok, sizeof(result)-strlen(result)-1);
                        first = 0;
                    }
                    tok = strtok_s(NULL, ",", &saveptr);
                }
                Value v; memset(&v, 0, sizeof(v));
                v.type = VAL_STR;
                strncpy(v.str, result, sizeof(v.str)-1);
                doc_set_field(c->docs[i], op->field, v);
            }
        }
        affected++;
    }
    db->dirty = 1;
    r->count = affected;
    if (affected == 0) {
        set_err(r, "NOT_FOUND", "no document matches condition");
    } else {
        set_info(r, "updated %d", affected);
    }
}

static void exec_demlet(HugoDatabase *db, const Query *q, HugoResult *r) {
    Collection *c = db_get_collection(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }

    int affected = 0;
    for (int i = 0; i < c->capacity; i++) {
        if (!c->docs[i]) continue;
        if (eval_condition(q->haar, c->docs[i])) {
            coll_delete(c, (uint64_t)(i + 1));
            affected++;
        }
    }
    db->dirty = 1;
    r->count = affected;
    set_info(r, "deleted %d", affected);
}

static void exec_madeco(HugoDatabase *db, const Query *q, HugoResult *r) {
    if (db_get_collection(db, q->collection)) {
        set_err(r, "EXISTS", "collection already exists");
        return;
    }
    if (!db_create_collection(db, q->collection)) {
        set_err(r, "MAX_COLLECTIONS", "too many collections");
        return;
    }
    db->dirty = 1;
    set_info(r, "created collection %s", q->collection);
}

static void exec_delco(HugoDatabase *db, const Query *q, HugoResult *r) {
    if (db_drop_collection(db, q->collection) != 0) {
        set_err(r, "NO_COLLECTION", "collection not found");
        return;
    }
    db->dirty = 1;
    set_info(r, "dropped collection %s", q->collection);
}

static void exec_skill(HugoDatabase *db, const Query *q, HugoResult *r) {
    if (q->collection[0] == 0) {
        /* skill (no arg) → list collections */
        char buf[1024] = {0};
        size_t off = 0;
        off += snprintf(buf + off, sizeof(buf) - off, "collections: ");
        for (int i = 0; i < db->n_collections; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%s",
                            i == 0 ? "" : ", ",
                            db->collections[i].name);
        }
        strncpy(r->info, buf, sizeof(r->info)-1);
        r->count = db->n_collections;
    } else {
        /* skill <coll> → document count */
        Collection *c = db_get_collection(db, q->collection);
        if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
        r->count = c->count;
        set_info(r, "collection %s: %d documents", q->collection, c->count);
    }
}

/* ===== Aggregation ===== */
typedef struct AggGroup {
    Value   key;            /* group key */
    int     count;          /* for pou */
    double  sums[16];       /* for sep */
    double  mins[16];       /* for mie */
    double  maxs[16];       /* for maf */
    int     inited[16];     /* min/max initialized? */
    struct AggGroup *next;
} AggGroup;

static void exec_gomail(HugoDatabase *db, const Query *q, HugoResult *r) {
    Collection *c = db_get_collection(db, q->collection);
    if (!c) { set_err(r, "NO_COLLECTION", "collection not found"); return; }
    if (!q->gremb_bi) { set_err(r, "NO_GROUP", "gomail requires gremb bi"); return; }

    AggGroup *groups = NULL;
    int n_groups = 0;

    /* Scan + group */
    for (int i = 0; i < c->capacity; i++) {
        if (!c->docs[i]) continue;
        if (!eval_condition(q->haar, c->docs[i])) continue;

        /* Get group key */
        Value gk;
        if (doc_get_field(c->docs[i], q->gremb_bi->field, &gk) != 0) {
            gk.type = VAL_NULL;
        }

        /* Find or create group */
        AggGroup *g = NULL;
        for (AggGroup *p = groups; p; p = p->next) {
            if (value_compare(&p->key, &gk) == 0) { g = p; break; }
        }
        if (!g) {
            g = (AggGroup*)calloc(1, sizeof(AggGroup));
            g->key = gk;
            g->next = groups;
            groups = g;
            n_groups++;
        }
        g->count++;

        /* Accumulate agg values */
        for (int a = 0; a < q->gremb_bi->n_aggs; a++) {
            Value av;
            if (doc_get_field(c->docs[i], q->gremb_bi->agg_fields[a], &av) != 0) continue;
            double v = (av.type == VAL_NUM) ? av.num : 0;
            g->sums[a] += v;
            if (!g->inited[a] || v < g->mins[a]) g->mins[a] = v;
            if (!g->inited[a] || v > g->maxs[a]) g->maxs[a] = v;
            g->inited[a] = 1;
        }
    }

    /* Build result documents */
    r->count = 0;
    for (AggGroup *g = groups; g && r->count < MAX_RESULT_DOCS; g = g->next) {
        Document *doc = (Document*)calloc(1, sizeof(Document));
        /* Group key */
        doc_set_field(doc, q->gremb_bi->field, g->key);
        /* Agg results */
        for (int a = 0; a < q->gremb_bi->n_aggs; a++) {
            Value v; memset(&v, 0, sizeof(v));
            v.type = VAL_NUM;
            char fname[256];
            const char *agg_name = "";
            switch (q->gremb_bi->agg_funcs[a]) {
            case TOK_POU: agg_name = "pou"; v.num = g->count; break;
            case TOK_SEP: agg_name = "sep"; v.num = g->sums[a]; break;
            case TOK_AWR: agg_name = "awr"; v.num = g->count > 0 ? g->sums[a] / g->count : 0; break;
            case TOK_MIE: agg_name = "mie"; v.num = g->mins[a]; break;
            case TOK_MAF: agg_name = "maf"; v.num = g->maxs[a]; break;
            default: break;
            }
            snprintf(fname, sizeof(fname), "%s_%s", agg_name, q->gremb_bi->agg_fields[a]);
            doc_set_field(doc, fname, v);
        }
        r->docs[r->count++] = doc;
    }

    /* Cleanup groups */
    AggGroup *g = groups;
    while (g) { AggGroup *n = g->next; free(g); g = n; }
}

/* ===== Main entry ===== */
int hugo_execute(HugoDatabase *db, const Query *q, HugoResult *r) {
    result_init(r);
    if (q->error[0]) { set_err(r, "PARSE_ERROR", q->error); return 0; }

    switch (q->verb) {
    case VERB_FUNDEN:   exec_funden(db, q, r);   break;
    case VERB_VIETINFO: exec_vietinfo(db, q, r); break;
    case VERB_COCHIN:   exec_cochin(db, q, r);   break;
    case VERB_DEMLET:   exec_demlet(db, q, r);   break;
    case VERB_MADECO:   exec_madeco(db, q, r);   break;
    case VERB_DELCO:    exec_delco(db, q, r);    break;
    case VERB_SKILL:    exec_skill(db, q, r);    break;
    case VERB_GOMAIL:   exec_gomail(db, q, r);   break;
    case VERB_GINAN:
    case VERB_COMETI:
    case VERB_TULABERK:
        set_info(r, "transaction %s (MVP: no-op)",
                 q->verb == VERB_GINAN ? "begin" :
                 q->verb == VERB_COMETI ? "commit" : "abort");
        break;
    case VERB_USF:
        set_info(r, "using database %s (MVP: single-db)", q->collection);
        break;
    case VERB_MADECOIDU:
    case VERB_DELECOIDU:
    case VERB_EXEPANUS:
        set_err(r, "NOT_IMPLEMENTED", "deferred to later phase");
        break;
    default:
        set_err(r, "UNKNOWN_VERB", "unknown verb");
    }
    return 0;
}

/* ===== Output formatter ===== */
static void print_value(const Value *v) {
    switch (v->type) {
    case VAL_NUM:
        if (v->num == (double)(long long)v->num)
            printf("%lld", (long long)v->num);
        else
            printf("%g", v->num);
        break;
    case VAL_STR:
        printf("\"%s\"", v->str);
        break;
    case VAL_BOOL:
        printf("%s", v->num != 0 ? "true" : "false");
        break;
    case VAL_NULL:
    default:
        printf("null");
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

void result_print(const HugoResult *r) {
    if (!r->ok) {
        printf("err %s \"%s\"\n", r->err_code, r->err_msg);
        return;
    }

    if (r->count > 0 && r->docs[0]) {
        /* Có danh sách doc */
        printf("ok %d document%s\n", r->count, r->count == 1 ? "" : "s");
        for (int i = 0; i < r->count; i++) {
            if (!r->docs[i]) continue;
            print_document(r->docs[i]);
            printf("\n");
        }
    } else {
        /* Message only */
        if (r->info[0]) printf("ok %s\n", r->info);
        else            printf("ok %d documents\n", r->count);
    }
}

