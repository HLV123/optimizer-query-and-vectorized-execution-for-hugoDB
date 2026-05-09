/* vec_agg.c — Vectorized aggregation */
#include "vec_agg.h"
#include "../core/collection.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

/* ===== FNV-1a hash ===== */
static uint32_t fnv_num(double v) {
    uint64_t bits; memcpy(&bits, &v, 8);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) {
        h ^= (uint8_t)(bits >> (i*8));
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv_str(const char *s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* ===== Slot lookup (open addressing, linear probe) ===== */
/* Trả về slot index cho key. Tạo slot mới nếu chưa có. */

static int find_or_create_num(VecAggTable *t, double key) {
    uint32_t mask = (uint32_t)(t->capacity - 1);
    uint32_t h    = fnv_num(key) & mask;
    for (int probe = 0; probe < t->capacity; probe++) {
        uint32_t slot = (h + probe) & mask;
        if (!t->slot_used[slot]) {
            /* Tạo group mới */
            t->slot_used[slot] = 1;
            t->key_num[slot]   = key;
            t->n_groups++;
            return (int)slot;
        }
        if (t->key_num[slot] == key) return (int)slot;
    }
    return -1; /* table full — không nên xảy ra với MAX_GROUPS đủ lớn */
}

static int find_or_create_str(VecAggTable *t, const char *key) {
    if (!key) key = "";
    uint32_t mask = (uint32_t)(t->capacity - 1);
    uint32_t h    = fnv_str(key) & mask;
    for (int probe = 0; probe < t->capacity; probe++) {
        uint32_t slot = (h + probe) & mask;
        if (!t->slot_used[slot]) {
            t->slot_used[slot] = 1;
            t->key_str[slot]   = (char*)key; /* pointer vào doc KVPair — stable */
            t->n_groups++;
            return (int)slot;
        }
        if (t->key_str[slot] && strcmp(t->key_str[slot], key) == 0) return (int)slot;
        if (!t->key_str[slot] && key[0] == '\0') return (int)slot;
    }
    return -1;
}

/* ===== vec_agg_new ===== */

VecAggTable* vec_agg_new(Arena *arena, const ColBatch *b,
                          const char *group_field,
                          const AggFunc *aggs, int n_aggs)
{
    if (!arena || !b) return NULL;
    if (n_aggs > VEC_AGG_MAX_AGGS) n_aggs = VEC_AGG_MAX_AGGS;

    VecAggTable *t = (VecAggTable*)arena_alloc(arena, sizeof(VecAggTable));
    if (!t) return NULL;
    memset(t, 0, sizeof(VecAggTable));

    /* Capacity = smallest power-of-2 >= 4 * expected groups.
     * Worst case: mỗi row một group → dùng n_rows * 2, cap ở MAX_GROUPS. */
    int cap = VEC_AGG_MAX_GROUPS;
    /* Nếu ít rows hơn, dùng cap nhỏ hơn để fit cache */
    for (cap = 64; cap < b->n_rows * 2 && cap < VEC_AGG_MAX_GROUPS; cap <<= 1);
    t->capacity = cap;

    /* Xác định group-by key type */
    ColType gtype;
    int gci = col_batch_find_col(b, group_field, &gtype);
    t->key_is_num = (gci >= 0 && gtype == COL_TYPE_NUM);
    strncpy(t->group_field, group_field, sizeof(t->group_field)-1);

    /* Alloc key arrays */
    if (t->key_is_num)
        t->key_num = (double*)arena_alloc(arena, cap * sizeof(double));
    else
        t->key_str = (char**)arena_alloc(arena, cap * sizeof(char*));
    t->slot_used = (uint8_t*)arena_alloc(arena, cap * sizeof(uint8_t));
    if (!t->slot_used) return NULL;
    memset(t->slot_used, 0, cap * sizeof(uint8_t));
    if (t->key_num)  memset(t->key_num,  0, cap * sizeof(double));
    if (t->key_str)  memset(t->key_str,  0, cap * sizeof(char*));

    /* Alloc accumulator arrays + map agg cols */
    t->n_aggs = n_aggs;
    for (int a = 0; a < n_aggs; a++) {
        t->agg_func[a] = aggs[a].func;
        strncpy(t->out_name[a], aggs[a].out_name, 255);

        /* Tìm col_idx cho agg field trong ColBatch */
        ColType atype;
        int aci = col_batch_find_col(b, aggs[a].field, &atype);
        t->agg_col[a] = (aci >= 0) ? b->cols[aci].col_idx : -1;

        t->sum[a]  = (double*)arena_alloc(arena, cap * sizeof(double));
        t->min_[a] = (double*)arena_alloc(arena, cap * sizeof(double));
        t->max_[a] = (double*)arena_alloc(arena, cap * sizeof(double));
        t->cnt[a]  = (int64_t*)arena_alloc(arena, cap * sizeof(int64_t));
        t->has[a]  = (uint8_t*)arena_alloc(arena, cap * sizeof(uint8_t));
        if (!t->sum[a] || !t->min_[a] || !t->max_[a] || !t->cnt[a] || !t->has[a])
            return NULL;
        memset(t->sum[a],  0, cap * sizeof(double));
        memset(t->cnt[a],  0, cap * sizeof(int64_t));
        memset(t->has[a],  0, cap * sizeof(uint8_t));
        /* min/max init: min=+INF, max=-INF */
        for (int s = 0; s < cap; s++) {
            t->min_[a][s] =  DBL_MAX;
            t->max_[a][s] = -DBL_MAX;
        }
    }

    return t;
}

/* ===== vec_agg_run ===== */

void vec_agg_run(VecAggTable *t, const ColBatch *b)
{
    if (!t || !b) return;

    /* Tìm group-by column index một lần */
    ColType gtype;
    int gci = col_batch_find_col(b, t->group_field, &gtype);
    int g_num_idx = -1, g_str_idx = -1;
    if (gci >= 0) {
        if (gtype == COL_TYPE_NUM) g_num_idx = b->cols[gci].col_idx;
        else                        g_str_idx = b->cols[gci].col_idx;
    }

    /* Main aggregation loop — inner loop không có malloc/free */
    for (int i = 0; i < b->n_rows; i++) {
        if (!b->alive[i]) continue;

        /* Tìm/tạo group slot */
        int slot;
        if (t->key_is_num) {
            double gkey = (g_num_idx >= 0) ? b->num_data[g_num_idx][i] : 0.0;
            slot = find_or_create_num(t, gkey);
        } else {
            char *gkey = (g_str_idx >= 0) ? b->str_data[g_str_idx][i] : NULL;
            slot = find_or_create_str(t, gkey ? gkey : "");
        }
        if (slot < 0) continue; /* table full — skip */

        /* Update accumulators cho từng agg function */
        for (int a = 0; a < t->n_aggs; a++) {
            int col = t->agg_col[a];

            /* COUNT(*) không cần field value */
            if (t->agg_func[a] == TOK_POU) {
                t->cnt[a][slot]++;
                continue;
            }

            if (col < 0) continue; /* field không có trong ColBatch */
            double v = b->num_data[col][i];
            if (b->null_mask[col][i]) continue; /* null — skip */

            t->sum[a][slot] += v;
            t->cnt[a][slot]++;
            if (!t->has[a][slot] || v < t->min_[a][slot]) t->min_[a][slot] = v;
            if (!t->has[a][slot] || v > t->max_[a][slot]) t->max_[a][slot] = v;
            t->has[a][slot] = 1;
        }
    }
}

/* ===== vec_agg_materialize ===== */

int vec_agg_materialize(const VecAggTable *t, Document **docs_out, int max_docs)
{
    if (!t || !docs_out) return 0;
    int written = 0;

    for (int s = 0; s < t->capacity && written < max_docs; s++) {
        if (!t->slot_used[s]) continue;

        Document *doc = (Document*)calloc(1, sizeof(Document));
        if (!doc) break;

        /* Group-by key */
        Value gv; memset(&gv, 0, sizeof(gv));
        if (t->key_is_num) {
            gv.type = VAL_NUM; gv.num = t->key_num[s];
        } else {
            gv.type = VAL_STR;
            if (t->key_str[s])
                strncpy(gv.str, t->key_str[s], sizeof(gv.str)-1);
        }
        doc_set_field(doc, t->group_field, gv);

        /* Agg result fields */
        for (int a = 0; a < t->n_aggs; a++) {
            Value rv; memset(&rv, 0, sizeof(rv)); rv.type = VAL_NUM;
            switch (t->agg_func[a]) {
            case TOK_POU: rv.num = (double)t->cnt[a][s]; break;
            case TOK_SEP: rv.num = t->sum[a][s]; break;
            case TOK_AWR:
                rv.num = (t->cnt[a][s] > 0) ? t->sum[a][s] / t->cnt[a][s] : 0.0;
                break;
            case TOK_MIE: rv.num = t->has[a][s] ? t->min_[a][s] : 0.0; break;
            case TOK_MAF: rv.num = t->has[a][s] ? t->max_[a][s] : 0.0; break;
            default: rv.num = 0.0; break;
            }
            doc_set_field(doc, t->out_name[a], rv);
        }

        docs_out[written++] = doc;
    }
    return written;
}

/* ===== Simple aggregates (no GROUP BY) ===== */

int64_t vec_agg_count_star(const ColBatch *b) {
    if (!b) return 0;
    int64_t c = 0;
    for (int i = 0; i < b->n_rows; i++) c += b->alive[i];
    return c;
}

double vec_agg_sum(const ColBatch *b, int num_col_idx) {
    if (!b || num_col_idx < 0) return 0.0;
    double s = 0.0;
    const double  *col = b->num_data[num_col_idx];
    const uint8_t *nm  = b->null_mask[num_col_idx];
    for (int i = 0; i < b->n_rows; i++)
        if (b->alive[i] && !nm[i]) s += col[i];
    return s;
}

double vec_agg_avg(const ColBatch *b, int num_col_idx) {
    if (!b || num_col_idx < 0) return 0.0;
    double s = 0.0; int64_t c = 0;
    const double  *col = b->num_data[num_col_idx];
    const uint8_t *nm  = b->null_mask[num_col_idx];
    for (int i = 0; i < b->n_rows; i++)
        if (b->alive[i] && !nm[i]) { s += col[i]; c++; }
    return c > 0 ? s / c : 0.0;
}

double vec_agg_min(const ColBatch *b, int num_col_idx) {
    if (!b || num_col_idx < 0) return 0.0;
    double m = DBL_MAX; int found = 0;
    const double  *col = b->num_data[num_col_idx];
    const uint8_t *nm  = b->null_mask[num_col_idx];
    for (int i = 0; i < b->n_rows; i++)
        if (b->alive[i] && !nm[i]) { if (!found || col[i]<m) m=col[i]; found=1; }
    return found ? m : 0.0;
}

double vec_agg_max(const ColBatch *b, int num_col_idx) {
    if (!b || num_col_idx < 0) return 0.0;
    double m = -DBL_MAX; int found = 0;
    const double  *col = b->num_data[num_col_idx];
    const uint8_t *nm  = b->null_mask[num_col_idx];
    for (int i = 0; i < b->n_rows; i++)
        if (b->alive[i] && !nm[i]) { if (!found || col[i]>m) m=col[i]; found=1; }
    return found ? m : 0.0;
}

/* ===== vec_agg_run_fast — interned string GROUP BY ===== */
#include "vec_str_intern.h"
#include <float.h>

void vec_agg_run_fast(VecAggTable *t, const ColBatch *b, Arena *arena)
{
    if (!t || !b) return;

    /* Numeric group-by: fall through to normal run */
    if (t->key_is_num) { vec_agg_run(t, b); return; }

    /* Find group-by string column */
    ColType gtype;
    int gci = col_batch_find_col(b, t->group_field, &gtype);
    if (gci < 0 || gtype != COL_TYPE_STR) { vec_agg_run(t, b); return; }

    int g_str_idx = b->cols[gci].col_idx;
    int mask_idx  = COL_BATCH_MAX_COLS / 2 + g_str_idx;

    /* ── Pass 1: intern strings → ids[] ── */
    StrIntern *si = str_intern_build(arena,
                                     b->str_data[g_str_idx],
                                     b->null_mask[mask_idx],
                                     b->n_rows);
    if (!si || si->n_unique > VEC_AGG_MAX_GROUPS) {
        vec_agg_run(t, b); /* fallback */
        return;
    }

    /* Register groups into VecAggTable */
    for (int k = 0; k < si->n_unique; k++) {
        /* find_or_create_str để set slot_used + key_str đúng */
        /* Thay vì gọi lại, điền trực tiếp vì table còn trống */
        t->slot_used[k] = 1;
        t->key_str[k]   = si->keys[k];
    }
    t->n_groups = si->n_unique;

    /* ── Pass 2: accumulate — inner loop chỉ dùng integer index ──
     * Compiler thấy: alive[i] (uint8_t), ids[i] (int32_t), num_data[col][i] (double)
     * → autovectorize với -O3 -march=native thành AVX2 gather/scatter */

    /* Pre-resolve agg col indices */
    int agg_col[VEC_AGG_MAX_AGGS];
    for (int a = 0; a < t->n_aggs; a++) agg_col[a] = t->agg_col[a];

    const int32_t *ids   = si->ids;
    const uint8_t *alive = b->alive;
    int n = b->n_rows;

    for (int a = 0; a < t->n_aggs; a++) {
        int col = agg_col[a];
        HugoTokenType func = t->agg_func[a];

        if (func == TOK_POU) {
            /* COUNT(*): accumulate count per group */
            int64_t *cnt = t->cnt[a];
            for (int i = 0; i < n; i++) {
                int32_t id = ids[i];
                cnt[id] += (alive[i] & (id >= 0));
            }
            continue;
        }

        if (col < 0) continue;
        const double  *vals = b->num_data[col];
        const uint8_t *nm   = b->null_mask[col];

        if (func == TOK_SEP) {
            /* SUM — simple accumulate, highly vectorizable */
            double  *sum = t->sum[a];
            int64_t *cnt = t->cnt[a];
            for (int i = 0; i < n; i++) {
                int32_t id = ids[i];
                int     ok = alive[i] & !nm[i] & (id >= 0);
                sum[id] += vals[i] * ok;
                cnt[id] += ok;
            }
        } else if (func == TOK_AWR) {
            /* AVG — same as SUM, divide at materialize */
            double  *sum = t->sum[a];
            int64_t *cnt = t->cnt[a];
            for (int i = 0; i < n; i++) {
                int32_t id = ids[i];
                int     ok = alive[i] & !nm[i] & (id >= 0);
                sum[id] += vals[i] * ok;
                cnt[id] += ok;
            }
        } else if (func == TOK_MIE) {
            /* MIN */
            double  *mn  = t->min_[a];
            uint8_t *has = t->has[a];
            for (int i = 0; i < n; i++) {
                int32_t id = ids[i];
                if (!alive[i] || nm[i] || id < 0) continue;
                if (!has[id] || vals[i] < mn[id]) { mn[id] = vals[i]; has[id] = 1; }
            }
        } else if (func == TOK_MAF) {
            /* MAX */
            double  *mx  = t->max_[a];
            uint8_t *has = t->has[a];
            for (int i = 0; i < n; i++) {
                int32_t id = ids[i];
                if (!alive[i] || nm[i] || id < 0) continue;
                if (!has[id] || vals[i] > mx[id]) { mx[id] = vals[i]; has[id] = 1; }
            }
        }
    }
}
