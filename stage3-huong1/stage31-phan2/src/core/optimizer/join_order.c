/* join_order.c — System R Dynamic Programming Join Order Optimizer
 *
 * Algorithm:
 *   1. Base case: for each single table, compute best access path (Seq/Index)
 *   2. For subsets of increasing size (2, 3, ..., N):
 *      For each split (S1, S2) of subset S:
 *        candidate = Join(best_plan[S1], best_plan[S2])
 *        if cost(candidate) < best_cost[S]: update dp[S]
 *   3. Return dp[full_set]
 *
 * Left-deep restriction: S1 is always a single table or already-joined set,
 * S2 is always a single table (right side = new table each step).
 * This reduces search space from O(3^N) to O(2^N).
 */
#include "join_order.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

/* ===== DP table operations ===== */

static uint32_t dp_hash(TableSet s) {
    /* FNV-1a style hash for TableSet */
    uint32_t h = s;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return h % DP_TABLE_SIZE;
}

static DPEntry* dp_get(DPTable *dp, TableSet s) {
    uint32_t h = dp_hash(s);
    /* Linear probe */
    for (int i = 0; i < DP_TABLE_SIZE; i++) {
        uint32_t idx = (h + i) % DP_TABLE_SIZE;
        if (!dp->entries[idx].valid) return NULL;
        if (dp->entries[idx].tables == s) return &dp->entries[idx];
    }
    return NULL;
}

static DPEntry* dp_put(DPTable *dp, TableSet s, PhysicalPlan *plan,
                        double cost, double est_rows) {
    uint32_t h = dp_hash(s);
    for (int i = 0; i < DP_TABLE_SIZE; i++) {
        uint32_t idx = (h + i) % DP_TABLE_SIZE;
        if (!dp->entries[idx].valid || dp->entries[idx].tables == s) {
            dp->entries[idx].tables   = s;
            dp->entries[idx].plan     = plan;
            dp->entries[idx].cost     = cost;
            dp->entries[idx].est_rows = est_rows;
            dp->entries[idx].valid    = 1;
            return &dp->entries[idx];
        }
    }
    return NULL; /* table full — shouldn't happen with DP_TABLE_SIZE=4096 */
}

/* ===== Helper functions ===== */

int tableset_popcount(TableSet s) {
    int c = 0;
    while (s) { c += s & 1; s >>= 1; }
    return c;
}

void enumerate_splits(TableSet s, split_callback_fn cb, void *user_ctx) {
    /* Enumerate all non-empty proper subsets s1 of s.
     * Use Gosper's hack to enumerate subsets efficiently.
     * Only call cb when s1 < (s ^ s1) to avoid duplicate (s1,s2) and (s2,s1). */
    TableSet sub = s;
    while (sub > 0) {
        sub = (sub - 1) & s;   /* next subset */
        if (sub == 0) break;
        TableSet complement = s ^ sub;
        if (sub < complement) { /* avoid duplicate pairs */
            cb(sub, complement, user_ctx);
        }
    }
}

/* Find table index by name */
static int find_table_idx(const JoinOrderCtx *ctx, const char *name) {
    for (int i = 0; i < (int)ctx->n_tables; i++)
        if (strcmp(ctx->tables[i], name) == 0) return i;
    return -1;
}

const JoinPredicate* find_join_pred(const JoinOrderCtx *ctx,
                                     TableSet s1, TableSet s2) {
    for (int i = 0; i < ctx->n_join_preds; i++) {
        const JoinPredicate *jp = &ctx->join_preds[i];
        TableSet t1 = 1U << jp->left_table_idx;
        TableSet t2 = 1U << jp->right_table_idx;
        /* Check if pred connects s1 and s2 */
        if (((t1 & s1) && (t2 & s2)) || ((t2 & s1) && (t1 & s2)))
            return jp;
    }
    return NULL;
}

/* ===== Base access path for single table ===== */

static PhysicalPlan* best_access_path(JoinOrderCtx *ctx, int table_idx,
                                       double *out_rows) {
    const char *coll = ctx->tables[table_idx];
    const Condition *filter = ctx->table_filters[table_idx];
    CollectionStats *cs = stats_get(ctx->stats, coll);
    CostModel *m = ctx->cost_model;
    Arena *arena = ctx->arena;

    double total_rows = cs ? (double)cs->total_rows : 1000.0;
    double page_count = cs ? (double)cs->page_count : 100.0;

    /* Always: SeqScan */
    PhysicalPlan *seq = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
    memset(seq, 0, sizeof(PhysicalPlan));
    seq->type = POP_SEQ_SCAN;
    strncpy(seq->seq_scan.collection_name, coll,
            sizeof(seq->seq_scan.collection_name) - 1);
    seq->estimated_cost = page_count * m->io_cost_per_page +
                          total_rows * m->cpu_cost_per_tuple;
    seq->estimated_rows = total_rows;

    PhysicalPlan *best = seq;
    double best_rows = total_rows;

    /* Check for IndexScan if filter available */
    if (filter && filter->type == COND_CMP) {
        DiskColl *dc = ddb_get_coll(ctx->db, coll);
        if (dc) {
            for (int i = 0; i < dc->n_indexes; i++) {
                if (strcmp(dc->indexes[i].field, filter->field) == 0) {
                    double sel = stats_estimate_selectivity(cs, filter);
                    double matching = total_rows * sel;
                    double idx_cost = (log(total_rows + 1) / log(100.0)) *
                                      m->io_cost_per_page +
                                      matching * m->io_cost_per_page +
                                      matching * m->cpu_cost_per_tuple;
                    if (idx_cost < seq->estimated_cost) {
                        PhysicalPlan *idx = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
                        memset(idx, 0, sizeof(PhysicalPlan));
                        idx->type = POP_INDEX_SCAN;
                        strncpy(idx->index_scan.collection_name, coll,
                                sizeof(idx->index_scan.collection_name) - 1);
                        strncpy(idx->index_scan.index_col, filter->field,
                                sizeof(idx->index_scan.index_col) - 1);
                        idx->index_scan.asc = 1;
                        idx->estimated_cost = idx_cost;
                        idx->estimated_rows = matching < 1 ? 1 : matching;
                        best = idx;
                        best_rows = idx->estimated_rows;
                    }
                    break;
                }
            }
        }
    }

    /* Wrap with Filter if needed */
    if (filter) {
        PhysicalPlan *filt = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
        memset(filt, 0, sizeof(PhysicalPlan));
        filt->type = POP_FILTER;
        filt->filter.predicate = filter;
        filt->left = best;
        double sel = stats_estimate_selectivity(cs, filter);
        filt->estimated_rows = best->estimated_rows * sel;
        if (filt->estimated_rows < 1) filt->estimated_rows = 1;
        filt->estimated_cost = best->estimated_cost +
                               filt->estimated_rows * m->cpu_cost_per_operator;
        best = filt;
        best_rows = filt->estimated_rows;
    }

    *out_rows = best_rows;
    return best;
}

/* ===== Build join plan between two sub-plans ===== */

typedef struct {
    JoinOrderCtx *ctx;
    TableSet      s;
    double        best_cost;
    PhysicalPlan *best_plan;
    double        best_rows;
} SplitCtx;

static void try_split(TableSet s1, TableSet s2, void *user_ctx) {
    SplitCtx *sc = (SplitCtx*)user_ctx;
    JoinOrderCtx *ctx = sc->ctx;

    DPEntry *left_entry  = dp_get(&ctx->dp, s1);
    DPEntry *right_entry = dp_get(&ctx->dp, s2);
    if (!left_entry || !right_entry) return;

    /* Find join predicate connecting s1 and s2 */
    const JoinPredicate *jp = find_join_pred(ctx, s1, s2);

    double l_rows = left_entry->est_rows;
    double r_rows = right_entry->est_rows;
    double l_cost = left_entry->cost;
    double r_cost = right_entry->cost;
    CostModel *m  = ctx->cost_model;
    Arena *arena  = ctx->arena;

    /* Estimate join output rows */
    double join_rows;
    if (jp) {
        /* Use stats for join cardinality */
        CollectionStats *lcs = NULL, *rcs = NULL;
        /* Get stats for single-table subsets */
        for (int i = 0; i < (int)ctx->n_tables; i++) {
            if (s1 == (1U << i)) lcs = stats_get(ctx->stats, ctx->tables[i]);
            if (s2 == (1U << i)) rcs = stats_get(ctx->stats, ctx->tables[i]);
        }
        join_rows = stats_estimate_join_cardinality(lcs, rcs,
                                                     jp->left_col, jp->right_col);
        /* Floor at 1 */
        if (join_rows < 1) join_rows = 1;
    } else {
        /* Cartesian product — expensive but allowed */
        join_rows = l_rows * r_rows;
    }

    /* Try each join algorithm */
    typedef struct { PhysicalOpType type; double join_cost; } JoinAlg;
    JoinAlg algs[3];
    int n_algs = 0;

    /* Nested Loop Join — always available */
    algs[n_algs].type = POP_NESTED_LOOP_JOIN;
    algs[n_algs].join_cost = cost_nested_loop_join(m, l_rows, r_rows);
    n_algs++;

    /* Hash Join — only for equality predicates */
    if (jp) {
        algs[n_algs].type = POP_HASH_JOIN;
        algs[n_algs].join_cost = cost_hash_join(m, r_rows, l_rows, 128.0);
        n_algs++;
    }

    /* Sort-Merge Join — only for equality predicates */
    if (jp) {
        algs[n_algs].type = POP_SORT_MERGE_JOIN;
        algs[n_algs].join_cost = cost_sort_merge_join(m, l_rows, r_rows, 0, 0, 128.0);
        n_algs++;
    }

    for (int a = 0; a < n_algs; a++) {
        double total_cost = l_cost + r_cost + algs[a].join_cost;
        if (total_cost < sc->best_cost) {
            PhysicalPlan *jplan = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
            memset(jplan, 0, sizeof(PhysicalPlan));
            jplan->type = algs[a].type;
            jplan->left  = left_entry->plan;
            jplan->right = right_entry->plan;
            jplan->estimated_cost = total_cost;
            jplan->estimated_rows = join_rows;

            if (jp) {
                /* Determine which col belongs to left vs right side */
                TableSet left_t  = 1U << jp->left_table_idx;
                if (s1 & left_t) {
                    strncpy(jplan->join.left_col,  jp->left_col,  127);
                    strncpy(jplan->join.right_col, jp->right_col, 127);
                } else {
                    strncpy(jplan->join.left_col,  jp->right_col, 127);
                    strncpy(jplan->join.right_col, jp->left_col,  127);
                }
            }

            sc->best_cost = total_cost;
            sc->best_plan = jplan;
            sc->best_rows = join_rows;

            if (ctx->trace) {
                printf("[join_dp] split (0x%x,0x%x) alg=%s cost=%.2f rows=%.0f\n",
                       s1, s2, 
                       algs[a].type == POP_HASH_JOIN ? "HashJoin" :
                       algs[a].type == POP_SORT_MERGE_JOIN ? "SMJoin" : "NLJoin",
                       total_cost, join_rows);
            }
        }
    }
}

/* ===== Context initialization ===== */

void join_order_ctx_init(JoinOrderCtx *ctx, DiskDB *db, StatsStore *stats,
                          CostModel *model, Arena *arena, int trace) {
    memset(ctx, 0, sizeof(JoinOrderCtx));
    ctx->db         = db;
    ctx->stats      = stats;
    ctx->cost_model = model;
    ctx->arena      = arena;
    ctx->trace      = trace;
}

int join_order_add_table(JoinOrderCtx *ctx, const char *table_name,
                          const Condition *filter) {
    if (ctx->n_tables >= MAX_JOIN_TABLES) return -1;
    int idx = (int)ctx->n_tables;
    ctx->tables[idx]        = table_name;
    ctx->table_filters[idx] = filter;
    ctx->n_tables++;
    return idx;
}

int join_order_add_predicate(JoinOrderCtx *ctx,
                              const char *left_table, const char *left_col,
                              const char *right_table, const char *right_col) {
    int li = find_table_idx(ctx, left_table);
    int ri = find_table_idx(ctx, right_table);
    if (li < 0 || ri < 0) return -1;

    int n = ctx->n_join_preds;
    if (n >= MAX_JOIN_TABLES * MAX_JOIN_TABLES) return -1;

    ctx->join_preds[n].left_table_idx  = li;
    ctx->join_preds[n].right_table_idx = ri;
    strncpy(ctx->join_preds[n].left_col,  left_col,  127);
    strncpy(ctx->join_preds[n].right_col, right_col, 127);
    ctx->n_join_preds++;
    return 0;
}

/* ===== Main DP algorithm ===== */

PhysicalPlan* join_order_optimize(JoinOrderCtx *ctx) {
    int n = (int)ctx->n_tables;
    if (n == 0) return NULL;
    if (n == 1) {
        /* Single table — just return best access path */
        double rows;
        PhysicalPlan *p = best_access_path(ctx, 0, &rows);
        dp_put(&ctx->dp, 1U, p, p->estimated_cost, rows);
        return p;
    }

    /* ===== Base case: single tables ===== */
    for (int i = 0; i < n; i++) {
        TableSet singleton = 1U << i;
        double rows;
        PhysicalPlan *p = best_access_path(ctx, i, &rows);
        dp_put(&ctx->dp, singleton, p, p->estimated_cost, rows);
        if (ctx->trace)
            printf("[join_dp] base table[%d]=%s cost=%.2f rows=%.0f\n",
                   i, ctx->tables[i], p->estimated_cost, rows);
    }

    /* ===== Inductive case: subsets of increasing size ===== */
    /* Left-deep restriction: enumerate subsets where the RIGHT side is always
     * a single table. This is O(N × 2^N) instead of O(3^N) for bushy. */
    for (int size = 2; size <= n; size++) {
        /* Enumerate all subsets of size `size` using Gosper's hack */
        TableSet full = (1U << n) - 1;
        /* Start with lowest `size` bits set */
        TableSet s = (1U << size) - 1;

        while (s <= full) {
            if ((s & full) == s) {
                /* Valid subset — try all left-deep splits:
                 * left = s minus one table, right = that single table */
                SplitCtx sc;
                sc.ctx       = ctx;
                sc.s         = s;
                sc.best_cost = DBL_MAX;
                sc.best_plan = NULL;
                sc.best_rows = 0;

                /* Left-deep: right side is always a single table */
                for (int i = 0; i < n; i++) {
                    TableSet single = 1U << i;
                    if (!(s & single)) continue;  /* table i not in subset s */
                    TableSet left_set = s ^ single; /* remove table i from s */
                    if (left_set == 0) continue;

                    DPEntry *left_entry = dp_get(&ctx->dp, left_set);
                    if (!left_entry) continue;

                    /* Build temporary right entry for single table */
                    DPEntry *right_entry = dp_get(&ctx->dp, single);
                    if (!right_entry) continue;

                    /* Try this split */
                    try_split(left_set, single, &sc);
                }

                if (sc.best_plan) {
                    dp_put(&ctx->dp, s, sc.best_plan,
                           sc.best_cost, sc.best_rows);
                    if (ctx->trace)
                        printf("[join_dp] subset=0x%x best_cost=%.2f rows=%.0f alg=%s\n",
                               s, sc.best_cost, sc.best_rows,
                               sc.best_plan->type == POP_HASH_JOIN ? "HashJoin" :
                               sc.best_plan->type == POP_SORT_MERGE_JOIN ? "SMJoin" : "NLJoin");
                }
            }

            /* Gosper's hack: next subset of same size */
            if (s == 0) break;
            TableSet c_val = s & (-s);
            TableSet r_val = s + c_val;
            s = (((r_val ^ s) >> 2) / c_val) | r_val;
        }
    }

    /* Return best plan for full set of all tables */
    TableSet full_set = (1U << n) - 1;
    DPEntry *result = dp_get(&ctx->dp, full_set);
    if (!result) {
        if (ctx->trace)
            printf("[join_dp] WARNING: no valid plan for full set 0x%x\n", full_set);
        return NULL;
    }
    if (ctx->trace)
        printf("[join_dp] final plan cost=%.2f rows=%.0f\n",
               result->cost, result->est_rows);
    return result->plan;
}
