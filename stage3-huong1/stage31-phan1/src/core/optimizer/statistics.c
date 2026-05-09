/* statistics.c â€” Build and persist collection statistics */
#include "statistics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===== Helpers ===== */

static CollectionStats* stats_find_or_create(StatsStore *ss, const char *name) {
    for (int i = 0; i < ss->n_entries; i++)
        if (strcmp(ss->entries[i].collection_name, name) == 0)
            return &ss->entries[i];
    if (ss->n_entries >= MAX_STATS_COLLECTIONS) return NULL;
    CollectionStats *cs = &ss->entries[ss->n_entries++];
    memset(cs, 0, sizeof(CollectionStats));
    strncpy(cs->collection_name, name, sizeof(cs->collection_name) - 1);
    return cs;
}

/* ===== Scan context for analyzing a collection ===== */

/* We collect all numeric values for each field, then build histogram.
 * For strings we do top-K counting. */

#define ANALYZE_MAX_FIELDS 64
#define ANALYZE_MAX_VALUES 100000  /* cap for histogram building */

#define EXACT_DISTINCT_MAX 256   /* exact distinct set for small cardinality */

typedef struct {
    char    field[128];
    /* numeric accumulation */
    double *num_values;
    int     num_count;
    int     num_cap;
    double  num_sum;
    double  num_min;
    double  num_max;
    int     num_inited;
    /* string top-K (simple: just count distinct) */
    char    top_vals[TOP_K_VALUES][128];
    uint64_t top_cnts[TOP_K_VALUES];
    int     n_top;
    /* general */
    uint64_t present_count;
    uint64_t null_count;
    /* distinct: exact small set + bloom filter overflow */
    double   exact_num_set[EXACT_DISTINCT_MAX];   /* exact numeric distinct values */
    int      exact_num_count;                      /* how many in exact set */
    int      exact_overflow;                       /* 1 = switched to bloom filter */
    uint64_t distinct_hash[4096];                  /* bit-set for overflow */
    uint64_t distinct_count;
} FieldAcc;

typedef struct {
    FieldAcc fields[ANALYZE_MAX_FIELDS];
    int      n_fields;
    uint64_t total_docs;
    uint64_t total_bytes;
} AnalyzeCtx;

static FieldAcc* acc_find_or_create(AnalyzeCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->n_fields; i++)
        if (strcmp(ctx->fields[i].field, name) == 0)
            return &ctx->fields[i];
    if (ctx->n_fields >= ANALYZE_MAX_FIELDS) return NULL;
    FieldAcc *fa = &ctx->fields[ctx->n_fields++];
    memset(fa, 0, sizeof(FieldAcc));
    strncpy(fa->field, name, sizeof(fa->field) - 1);
    fa->num_min = 1e300;
    fa->num_max = -1e300;
    return fa;
}

static void acc_push_num(FieldAcc *fa, double v) {
    if (fa->num_count < ANALYZE_MAX_VALUES) {
        if (!fa->num_values) {
            fa->num_cap = 1024;
            fa->num_values = (double*)malloc(fa->num_cap * sizeof(double));
        }
        if (fa->num_count >= fa->num_cap) {
            fa->num_cap *= 2;
            fa->num_values = (double*)realloc(fa->num_values, fa->num_cap * sizeof(double));
        }
        if (fa->num_values) fa->num_values[fa->num_count++] = v;
    }
    fa->num_sum += v;
    if (!fa->num_inited || v < fa->num_min) fa->num_min = v;
    if (!fa->num_inited || v > fa->num_max) fa->num_max = v;
    fa->num_inited = 1;
}

static void acc_push_str(FieldAcc *fa, const char *s) {
    /* Update top-K frequency count */
    for (int i = 0; i < fa->n_top; i++) {
        if (strcmp(fa->top_vals[i], s) == 0) {
            fa->top_cnts[i]++;
            return;
        }
    }
    if (fa->n_top < TOP_K_VALUES) {
        strncpy(fa->top_vals[fa->n_top], s, 127);
        fa->top_cnts[fa->n_top] = 1;
        fa->n_top++;
    } else {
        /* Evict minimum */
        int min_i = 0;
        for (int i = 1; i < fa->n_top; i++)
            if (fa->top_cnts[i] < fa->top_cnts[min_i]) min_i = i;
        strncpy(fa->top_vals[min_i], s, 127);
        fa->top_cnts[min_i] = 1;
    }
}

static void acc_track_distinct(FieldAcc *fa, const char *sval, double nval, int is_num) {
    if (!fa->exact_overflow) {
        if (is_num) {
            /* Check if already in exact set */
            for (int i = 0; i < fa->exact_num_count; i++)
                if (fa->exact_num_set[i] == nval) return; /* already seen */
            if (fa->exact_num_count < EXACT_DISTINCT_MAX) {
                fa->exact_num_set[fa->exact_num_count++] = nval;
                fa->distinct_count++;
                return;
            }
        } else {
            /* For strings: top_k doubles as small distinct set */
            for (int i = 0; i < fa->n_top; i++)
                if (strcmp(fa->top_vals[i], sval) == 0) return;
            if (fa->n_top < TOP_K_VALUES) {
                strncpy(fa->top_vals[fa->n_top], sval, 127);
                fa->top_cnts[fa->n_top] = 0;  /* count handled separately */
                fa->n_top++;
                fa->distinct_count++;
                return;
            }
        }
        /* Overflow to bloom filter */
        fa->exact_overflow = 1;
    }
    /* Bloom filter path */
    uint64_t h1, h2;
    if (is_num) {
        uint64_t bits; memcpy(&bits, &nval, sizeof(bits));
        h1 = bits ^ (bits >> 17); h2 = bits ^ (bits >> 23) ^ 0xDEADBEEFULL;
    } else {
        h1 = 14695981039346656037ULL; h2 = 2166136261ULL;
        for (const char *p = sval; *p; p++) {
            h1 ^= (unsigned char)*p; h1 *= 1099511628211ULL;
            h2 ^= (unsigned char)*p; h2 *= 16777619ULL;
        }
    }
    uint64_t i1 = h1 % 4096, i2 = h2 % 4096;
    int b1 = !!(fa->distinct_hash[i1/64] & (1ULL << (i1%64)));
    int b2 = !!(fa->distinct_hash[i2/64] & (1ULL << (i2%64)));
    fa->distinct_hash[i1/64] |= (1ULL << (i1%64));
    fa->distinct_hash[i2/64] |= (1ULL << (i2%64));
    if (!b1 && !b2) fa->distinct_count++;
}

static void analyze_visit(uint64_t id, Document *doc, void *ctx_) {
    (void)id;
    AnalyzeCtx *ctx = (AnalyzeCtx*)ctx_;
    ctx->total_docs++;

    /* Rough byte estimate: count chars in fields */
    size_t doc_bytes = 16; /* overhead */
    for (KVPair *kv = doc->pairs; kv; kv = kv->next) {
        doc_bytes += strlen(kv->key) + 32;
        FieldAcc *fa = acc_find_or_create(ctx, kv->key);
        if (!fa) continue;
        fa->present_count++;
        if (kv->value.type == VAL_NUM) {
            acc_push_num(fa, kv->value.num);
            acc_track_distinct(fa, NULL, kv->value.num, 1);
            doc_bytes += 8;
        } else if (kv->value.type == VAL_STR) {
            acc_push_str(fa, kv->value.str);
            acc_track_distinct(fa, kv->value.str, 0, 0);
            doc_bytes += strlen(kv->value.str);
        } else {
            fa->null_count++;
        }
    }
    ctx->total_bytes += doc_bytes;
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static void build_histogram(FieldAcc *fa, ColumnStats *cs) {
    if (!fa->num_values || fa->num_count < 2) {
        cs->has_histogram = 0;
        return;
    }
    /* Sort */
    qsort(fa->num_values, fa->num_count, sizeof(double), compare_double);

    int n = fa->num_count;
    /* Clamp buckets: if fewer data points than HISTOGRAM_BUCKETS, use n buckets */
    int K = (n < HISTOGRAM_BUCKETS) ? n : HISTOGRAM_BUCKETS;
    if (K < 1) { cs->has_histogram = 0; return; }

    /* Equi-depth: ~n/K per bucket */
    cs->histogram_bounds[0] = fa->num_values[0];
    for (int i = 0; i < K; i++) {
        int end_idx = ((i + 1) * n) / K - 1;
        if (end_idx < 0)  end_idx = 0;
        if (end_idx >= n) end_idx = n - 1;
        cs->histogram_bounds[i + 1] = fa->num_values[end_idx];
        cs->histogram_counts[i]     = (uint64_t)(n / K);
    }
    /* Last bucket gets remainder */
    cs->histogram_counts[K - 1] += (uint64_t)(n - (n / K) * K);
    /* Zero-fill remaining bounds if K < HISTOGRAM_BUCKETS */
    for (int i = K; i <= HISTOGRAM_BUCKETS; i++)
        cs->histogram_bounds[i] = fa->num_values[n - 1];
    cs->has_histogram = 1;
}

/* ===== Public API ===== */

void stats_store_init(StatsStore *ss, const char *db_path) {
    memset(ss, 0, sizeof(StatsStore));
    snprintf(ss->stats_path, sizeof(ss->stats_path), "%s.stats", db_path);
}

int stats_analyze(DiskDB *db, StatsStore *ss, const char *collection) {
    DiskColl *c = ddb_get_coll(db, collection);
    if (!c) return -1;

    AnalyzeCtx *ctx = (AnalyzeCtx*)calloc(1, sizeof(AnalyzeCtx));
    if (!ctx) return -1;

    ddb_scan(db, c, analyze_visit, ctx);

    CollectionStats *cs = stats_find_or_create(ss, collection);
    if (!cs) return -1;

    cs->total_rows   = ctx->total_docs;
    cs->total_bytes  = ctx->total_bytes;
    cs->page_count   = (ctx->total_bytes / 4096) + 1;
    cs->avg_row_size = ctx->total_docs > 0 ? ctx->total_bytes / ctx->total_docs : 128;
    cs->is_valid     = 1;
    cs->n_columns    = 0;

    for (int i = 0; i < ctx->n_fields && cs->n_columns < MAX_STATS_COLUMNS; i++) {
        FieldAcc *fa = &ctx->fields[i];
        ColumnStats *col = &cs->columns[cs->n_columns++];
        memset(col, 0, sizeof(ColumnStats));
        strncpy(col->column_name, fa->field, sizeof(col->column_name) - 1);
        col->total_rows    = fa->present_count;
        col->null_count    = fa->null_count;
        /* Distinct count: use histogram to count unique boundary values (stable).
         * Fall back to bloom filter estimate if no histogram. */
        {
            uint64_t bd = fa->distinct_count > 0 ? fa->distinct_count : 1;
            if (col->has_histogram) {
                /* Count unique adjacent boundary values â€” reliable estimate */
                int ndist = 1;
                for (int b = 1; b <= HISTOGRAM_BUCKETS; b++) {
                    if (col->histogram_bounds[b] > col->histogram_bounds[b-1] + 1e-9)
                        ndist++;
                }
                /* Take max of bloom filter estimate and histogram-derived estimate */
                if ((uint64_t)ndist > bd) bd = (uint64_t)ndist;
            }
            /* Also floor at number of top-K string values found */
            if ((uint64_t)col->n_top_k > bd) bd = (uint64_t)col->n_top_k;
            col->distinct_count = bd;
        }
        col->min_value     = fa->num_min;
        col->max_value     = fa->num_max;
        col->avg_value     = fa->present_count > 0 ? fa->num_sum / fa->present_count : 0;

        /* Histogram */
        build_histogram(fa, col);

        /* Top-K strings */
        col->n_top_k = fa->n_top;
        for (int k = 0; k < fa->n_top; k++) {
            strncpy(col->top_k_values[k], fa->top_vals[k], 127);
            col->top_k_counts[k] = fa->top_cnts[k];
        }

        /* Free accumulated numeric values */
        free(fa->num_values);
    }

    free(ctx);
    return 0;
}

CollectionStats* stats_get(StatsStore *ss, const char *collection) {
    for (int i = 0; i < ss->n_entries; i++) {
        if (strcmp(ss->entries[i].collection_name, collection) == 0 &&
            ss->entries[i].is_valid)
            return &ss->entries[i];
    }
    return NULL;
}

ColumnStats* stats_find_column(CollectionStats *cs, const char *col_name) {
    if (!cs) return NULL;
    for (int i = 0; i < cs->n_columns; i++)
        if (strcmp(cs->columns[i].column_name, col_name) == 0)
            return &cs->columns[i];
    return NULL;
}

/* ===== Selectivity estimation ===== */

static double selectivity_cmp(ColumnStats *col, HugoTokenType op, const Value *val) {
    if (!col) return 0.1; /* fallback */
    double distinct = (double)col->distinct_count;
    if (distinct < 1) distinct = 1;
    double total = (double)col->total_rows;
    if (total < 1) total = 1;

    switch (op) {
    case TOK_OP_BG: /* equality */
        /* If top-K has this value, use exact count */
        if (val->type == VAL_STR) {
            for (int i = 0; i < col->n_top_k; i++)
                if (strcmp(col->top_k_values[i], val->str) == 0)
                    return (double)col->top_k_counts[i] / total;
        }
        return 1.0 / distinct;

    case TOK_OP_KC: /* not equal */
        return 1.0 - 1.0 / distinct;

    case TOK_OP_LH:  /* < */
    case TOK_OP_LHB: /* <= */
    {
        if (!col->has_histogram || val->type != VAL_NUM) return 0.3;
        double v = val->num;
        if (v <= col->histogram_bounds[0]) return 0.0;
        if (v >= col->histogram_bounds[HISTOGRAM_BUCKETS]) return 1.0;
        double sel = 0;
        for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
            double lo = col->histogram_bounds[b];
            double hi = col->histogram_bounds[b + 1];
            if (v <= lo) break;
            if (v >= hi) {
                sel += (double)col->histogram_counts[b] / total;
            } else {
                /* Partial bucket */
                double frac = (hi > lo) ? (v - lo) / (hi - lo) : 0.5;
                sel += frac * (double)col->histogram_counts[b] / total;
                break;
            }
        }
        return sel < 0 ? 0 : (sel > 1 ? 1 : sel);
    }

    case TOK_OP_BH:  /* > */
    case TOK_OP_BHB: /* >= */
    {
        if (!col->has_histogram || val->type != VAL_NUM) return 0.3;
        double v = val->num;
        if (v >= col->histogram_bounds[HISTOGRAM_BUCKETS]) return 0.0;
        if (v <= col->histogram_bounds[0]) return 1.0;
        double sel = 0;
        for (int b = HISTOGRAM_BUCKETS - 1; b >= 0; b--) {
            double lo = col->histogram_bounds[b];
            double hi = col->histogram_bounds[b + 1];
            if (v >= hi) break;
            if (v <= lo) {
                sel += (double)col->histogram_counts[b] / total;
            } else {
                double frac = (hi > lo) ? (hi - v) / (hi - lo) : 0.5;
                sel += frac * (double)col->histogram_counts[b] / total;
                break;
            }
        }
        return sel < 0 ? 0 : (sel > 1 ? 1 : sel);
    }

    case TOK_OP_XAU: /* contains substring */
        return 0.1;

    default:
        return 0.1;
    }
}

double stats_estimate_selectivity(const CollectionStats *cs, const Condition *pred) {
    if (!pred) return 1.0;
    switch (pred->type) {
    case COND_AND:
        /* Independence assumption */
        return stats_estimate_selectivity(cs, pred->left) *
               stats_estimate_selectivity(cs, pred->right);
    case COND_OR: {
        double sa = stats_estimate_selectivity(cs, pred->left);
        double sb = stats_estimate_selectivity(cs, pred->right);
        return sa + sb - sa * sb;
    }
    case COND_NOT:
        return 1.0 - stats_estimate_selectivity(cs, pred->left);
    case COND_EXISTS:
        return cs ? (1.0 - (double)0 / (cs->total_rows > 0 ? cs->total_rows : 1)) : 0.9;
    case COND_IN:
        return pred->n_values > 0 ?
               (double)pred->n_values / (cs && cs->total_rows > 0 ? cs->total_rows : 100) :
               0.0;
    case COND_CMP: {
        if (!cs) return 0.1;
        ColumnStats *col = NULL;
        for (int i = 0; i < cs->n_columns; i++)
            if (strcmp(cs->columns[i].column_name, pred->field) == 0) {
                col = (ColumnStats*)&cs->columns[i];
                break;
            }
        return selectivity_cmp(col, pred->op, &pred->value);
    }
    default:
        return 0.1;
    }
}

double stats_estimate_join_cardinality(
    const CollectionStats *left_stats,
    const CollectionStats *right_stats,
    const char *left_col,
    const char *right_col)
{
    double left_rows  = left_stats  ? (double)left_stats->total_rows  : 1000.0;
    double right_rows = right_stats ? (double)right_stats->total_rows : 1000.0;

    /* Get distinct counts for join columns */
    double left_distinct = left_rows;
    double right_distinct = right_rows;

    if (left_stats) {
        for (int i = 0; i < left_stats->n_columns; i++)
            if (strcmp(left_stats->columns[i].column_name, left_col) == 0) {
                left_distinct = (double)left_stats->columns[i].distinct_count;
                break;
            }
    }
    if (right_stats) {
        for (int i = 0; i < right_stats->n_columns; i++)
            if (strcmp(right_stats->columns[i].column_name, right_col) == 0) {
                right_distinct = (double)right_stats->columns[i].distinct_count;
                break;
            }
    }

    /* Join selectivity = 1 / max(distinct_left, distinct_right) */
    double join_distinct = left_distinct > right_distinct ? left_distinct : right_distinct;
    if (join_distinct < 1) join_distinct = 1;

    return left_rows * right_rows / join_distinct;
}

/* ===== Persistence ===== */

int stats_persist(const StatsStore *ss) {
    FILE *f = fopen(ss->stats_path, "w");
    if (!f) return -1;

    fprintf(f, "HUGO_STATS_V1\n");
    fprintf(f, "collections %d\n", ss->n_entries);

    for (int i = 0; i < ss->n_entries; i++) {
        const CollectionStats *cs = &ss->entries[i];
        if (!cs->is_valid) continue;
        fprintf(f, "collection %s\n", cs->collection_name);
        fprintf(f, "total_rows %llu\n", (unsigned long long)cs->total_rows);
        fprintf(f, "total_bytes %llu\n", (unsigned long long)cs->total_bytes);
        fprintf(f, "page_count %llu\n", (unsigned long long)cs->page_count);
        fprintf(f, "avg_row_size %llu\n", (unsigned long long)cs->avg_row_size);
        fprintf(f, "n_columns %d\n", cs->n_columns);

        for (int j = 0; j < cs->n_columns; j++) {
            const ColumnStats *col = &cs->columns[j];
            fprintf(f, "col %s\n", col->column_name);
            fprintf(f, "col_total_rows %llu\n", (unsigned long long)col->total_rows);
            fprintf(f, "col_null_count %llu\n", (unsigned long long)col->null_count);
            fprintf(f, "col_distinct %llu\n", (unsigned long long)col->distinct_count);
            fprintf(f, "col_min %g\n", col->min_value);
            fprintf(f, "col_max %g\n", col->max_value);
            fprintf(f, "col_avg %g\n", col->avg_value);
            fprintf(f, "col_has_histogram %d\n", col->has_histogram);
            if (col->has_histogram) {
                fprintf(f, "histogram_bounds");
                for (int b = 0; b <= HISTOGRAM_BUCKETS; b++)
                    fprintf(f, " %g", col->histogram_bounds[b]);
                fprintf(f, "\n");
                fprintf(f, "histogram_counts");
                for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
                    fprintf(f, " %llu", (unsigned long long)col->histogram_counts[b]);
                fprintf(f, "\n");
            }
            fprintf(f, "col_n_top_k %d\n", col->n_top_k);
            for (int k = 0; k < col->n_top_k; k++)
                fprintf(f, "top_k %llu %s\n",
                        (unsigned long long)col->top_k_counts[k],
                        col->top_k_values[k]);
        }
        fprintf(f, "end_collection\n");
    }
    fclose(f);
    return 0;
}

int stats_load(StatsStore *ss) {
    FILE *f = fopen(ss->stats_path, "r");
    if (!f) return -1;

    char line[1024];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "HUGO_STATS_V1", 13) != 0) {
        fclose(f); return -1;
    }

    ss->n_entries = 0;
    CollectionStats *cur = NULL;
    ColumnStats *cur_col = NULL;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;

        if (strncmp(line, "collection ", 11) == 0) {
            cur = stats_find_or_create(ss, line + 11);
            cur_col = NULL;
            if (cur) cur->is_valid = 1;
        } else if (!cur) {
            continue;
        } else if (strncmp(line, "total_rows ", 11) == 0) {
            cur->total_rows = (uint64_t)strtoull(line + 11, NULL, 10);
        } else if (strncmp(line, "total_bytes ", 12) == 0) {
            cur->total_bytes = (uint64_t)strtoull(line + 12, NULL, 10);
        } else if (strncmp(line, "page_count ", 11) == 0) {
            cur->page_count = (uint64_t)strtoull(line + 11, NULL, 10);
        } else if (strncmp(line, "avg_row_size ", 13) == 0) {
            cur->avg_row_size = (uint64_t)strtoull(line + 13, NULL, 10);
        } else if (strncmp(line, "col ", 4) == 0) {
            if (cur->n_columns < MAX_STATS_COLUMNS) {
                cur_col = &cur->columns[cur->n_columns++];
                memset(cur_col, 0, sizeof(ColumnStats));
                strncpy(cur_col->column_name, line + 4, sizeof(cur_col->column_name) - 1);
            } else cur_col = NULL;
        } else if (cur_col) {
            if (strncmp(line, "col_total_rows ", 15) == 0)
                cur_col->total_rows = (uint64_t)strtoull(line + 15, NULL, 10);
            else if (strncmp(line, "col_null_count ", 15) == 0)
                cur_col->null_count = (uint64_t)strtoull(line + 15, NULL, 10);
            else if (strncmp(line, "col_distinct ", 13) == 0)
                cur_col->distinct_count = (uint64_t)strtoull(line + 13, NULL, 10);
            else if (strncmp(line, "col_min ", 8) == 0)
                cur_col->min_value = atof(line + 8);
            else if (strncmp(line, "col_max ", 8) == 0)
                cur_col->max_value = atof(line + 8);
            else if (strncmp(line, "col_avg ", 8) == 0)
                cur_col->avg_value = atof(line + 8);
            else if (strncmp(line, "col_has_histogram ", 18) == 0)
                cur_col->has_histogram = atoi(line + 18);
            else if (strncmp(line, "histogram_bounds ", 17) == 0) {
                char *p = line + 17;
                for (int b = 0; b <= HISTOGRAM_BUCKETS; b++) {
                    cur_col->histogram_bounds[b] = strtod(p, &p);
                    while (*p == ' ') p++;
                }
            } else if (strncmp(line, "histogram_counts ", 17) == 0) {
                char *p = line + 17;
                for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
                    cur_col->histogram_counts[b] = (uint64_t)strtoull(p, &p, 10);
                    while (*p == ' ') p++;
                }
            } else if (strncmp(line, "col_n_top_k ", 12) == 0) {
                cur_col->n_top_k = atoi(line + 12);
            } else if (strncmp(line, "top_k ", 6) == 0) {
                char *p = line + 6;
                uint64_t cnt = strtoull(p, &p, 10);
                while (*p == ' ') p++;
                int k = cur_col->n_top_k > 0 ? cur_col->n_top_k - 1 : 0;
                /* Find first empty slot */
                for (k = 0; k < TOP_K_VALUES; k++)
                    if (cur_col->top_k_values[k][0] == 0) break;
                if (k < TOP_K_VALUES) {
                    cur_col->top_k_counts[k] = cnt;
                    strncpy(cur_col->top_k_values[k], p, 127);
                }
            }
        }
    }
    fclose(f);
    return 0;
}

