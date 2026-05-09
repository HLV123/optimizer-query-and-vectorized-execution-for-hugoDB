/* optimizer.c — Hugo DB query optimizer (Phase 1-10)
 *
 * Implements cost-based selection of physical plans from logical plan.
 * Phase 7: Join Order DP (System R algorithm)
 * Phase 10: Advanced rules (constant folding, predicate splitting)
 */
#include "optimizer.h"
#include "join_order.h"
#include "advanced_rules.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

/* ===== Context lifecycle ===== */

void optimizer_ctx_init(OptimizerCtx *ctx, DiskDB *db, const char *db_path) {
    memset(ctx, 0, sizeof(OptimizerCtx));
    ctx->db   = db;
    ctx->mode = HUGO_OPT_COST_BASED;
    ctx->trace = 0;
    cost_model_init_default(&ctx->cost_model);
    stats_store_init(&ctx->stats, db_path);
    /* Try to load existing stats */
    stats_load(&ctx->stats);
}

void optimizer_ctx_free(OptimizerCtx *ctx) {
    /* StatsStore is stack-allocated inside OptimizerCtx, no heap to free */
    (void)ctx;
}

void optimizer_set_mode(OptimizerCtx *ctx, OptimizerMode mode) {
    ctx->mode = mode;
}

void optimizer_set_trace(OptimizerCtx *ctx, int trace) {
    ctx->trace = trace;
}

int optimizer_analyze(OptimizerCtx *ctx, const char *collection) {
    int rc = stats_analyze(ctx->db, &ctx->stats, collection);
    if (rc == 0) stats_persist(&ctx->stats);
    return rc;
}

/* ===== Physical plan builder helpers ===== */

static PhysicalPlan* pplan_new(Arena *arena, PhysicalOpType type) {
    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
    if (!p) return NULL;
    memset(p, 0, sizeof(PhysicalPlan));
    p->type = type;
    return p;
}

/* ===== Enumerate physical plans for a single logical node ===== */

/* Choose best access path for a Scan node.
 * Returns cheapest between SeqScan and IndexScan (if index available). */
static PhysicalPlan* choose_scan(OptimizerCtx *ctx, Arena *arena,
                                  const LogicalPlan *lscan,
                                  const Condition *filter_pred) {
    const char *coll = lscan->scan.collection_name;
    CollectionStats *cs = stats_get(&ctx->stats, coll);
    const CostModel *m  = &ctx->cost_model;

    /* Always available: SeqScan */
    PhysicalPlan *seq = pplan_new(arena, POP_SEQ_SCAN);
    if (!seq) return NULL;
    strncpy(seq->seq_scan.collection_name, coll,
            sizeof(seq->seq_scan.collection_name) - 1);
    seq->estimated_cost = cost_seq_scan(m, cs);
    seq->estimated_rows = cs ? (double)cs->total_rows : 1000.0;

    if (ctx->trace)
        printf("[opt] SeqScan(%s) cost=%.2f rows=%.0f\n",
               coll, seq->estimated_cost, seq->estimated_rows);

    /* Try IndexScan if there is a simple equality/range predicate on an indexed field */
    if (filter_pred && filter_pred->type == COND_CMP) {
        DiskColl *dc = ddb_get_coll(ctx->db, coll);
        if (dc) {
            for (int i = 0; i < dc->n_indexes; i++) {
                if (strcmp(dc->indexes[i].field, filter_pred->field) == 0) {
                    /* Index found — estimate cost */
                    double sel = stats_estimate_selectivity(cs, filter_pred);
                    PhysicalPlan *idx = pplan_new(arena, POP_INDEX_SCAN);
                    if (!idx) break;
                    strncpy(idx->index_scan.collection_name, coll,
                            sizeof(idx->index_scan.collection_name) - 1);
                    strncpy(idx->index_scan.index_col, filter_pred->field,
                            sizeof(idx->index_scan.index_col) - 1);
                    idx->index_scan.asc = 1;
                    idx->estimated_cost = cost_index_scan(m, cs, sel);
                    idx->estimated_rows = seq->estimated_rows * sel;
                    if (idx->estimated_rows < 1) idx->estimated_rows = 1;

                    if (ctx->trace)
                        printf("[opt] IndexScan(%s.%s) sel=%.3f cost=%.2f rows=%.0f\n",
                               coll, filter_pred->field, sel,
                               idx->estimated_cost, idx->estimated_rows);

                    /* Pick cheaper plan */
                    if (idx->estimated_cost < seq->estimated_cost) {
                        if (ctx->trace)
                            printf("[opt] → chose IndexScan (cheaper by %.2f)\n",
                                   seq->estimated_cost - idx->estimated_cost);
                        return idx;
                    }
                    break;
                }
            }
        }
    }
    return seq;
}

/* Forward declaration */
static PhysicalPlan* enumerate_physical(OptimizerCtx *ctx, Arena *arena,
                                         const LogicalPlan *lplan);

/* Build physical join, choosing best algorithm */
static PhysicalPlan* build_physical_join(OptimizerCtx *ctx, Arena *arena,
                                          const LogicalPlan *ljoin,
                                          PhysicalPlan *left_phys,
                                          PhysicalPlan *right_phys) {
    const CostModel *m = &ctx->cost_model;
    double l_rows = left_phys->estimated_rows;
    double r_rows = right_phys->estimated_rows;
    double avg_row_size = 128.0;

    double cost_nl  = cost_nested_loop_join(m, l_rows, r_rows);
    double cost_hj  = cost_hash_join(m, r_rows, l_rows, avg_row_size); /* build smaller */
    double cost_smj = cost_sort_merge_join(m, l_rows, r_rows, 0, 0, avg_row_size);

    if (ctx->trace) {
        printf("[opt] Join candidates: NL=%.2f HJ=%.2f SMJ=%.2f\n",
               cost_nl, cost_hj, cost_smj);
    }

    PhysicalOpType best_type = POP_NESTED_LOOP_JOIN;
    double best_cost = cost_nl;

    /* Hash join only valid for equality predicate */
    if (cost_hj < best_cost) {
        best_type = POP_HASH_JOIN;
        best_cost = cost_hj;
    }
    if (cost_smj < best_cost) {
        best_type = POP_SORT_MERGE_JOIN;
        best_cost = cost_smj;
    }

    PhysicalPlan *join = pplan_new(arena, best_type);
    if (!join) return NULL;
    strncpy(join->join.left_col,  ljoin->join.left_col,
            sizeof(join->join.left_col) - 1);
    strncpy(join->join.right_col, ljoin->join.right_col,
            sizeof(join->join.right_col) - 1);
    join->left  = left_phys;
    join->right = right_phys;
    /* Total cost = children cost + join cost */
    join->estimated_cost = left_phys->estimated_cost + right_phys->estimated_cost + best_cost;
    /* Join cardinality estimate */
    CollectionStats *lcs = NULL, *rcs = NULL;
    if (ljoin->left  && ljoin->left->type == LOP_SCAN)
        lcs = stats_get(&ctx->stats, ljoin->left->scan.collection_name);
    if (ljoin->right && ljoin->right->type == LOP_SCAN)
        rcs = stats_get(&ctx->stats, ljoin->right->scan.collection_name);
    join->estimated_rows = stats_estimate_join_cardinality(
        lcs, rcs, ljoin->join.left_col, ljoin->join.right_col);

    if (ctx->trace)
        printf("[opt] → chose %s for join, total_cost=%.2f rows=%.0f\n",
               pop_type_name(best_type), join->estimated_cost, join->estimated_rows);
    return join;
}

/* Main recursive physical plan builder */
static PhysicalPlan* enumerate_physical(OptimizerCtx *ctx, Arena *arena,
                                         const LogicalPlan *lplan) {
    if (!lplan) return NULL;
    const CostModel *m = &ctx->cost_model;

    switch (lplan->type) {
    case LOP_SCAN: {
        return choose_scan(ctx, arena, lplan, NULL);
    }

    case LOP_FILTER: {
        /* Try to push filter into scan (IndexScan optimization) */
        if (lplan->left && lplan->left->type == LOP_SCAN) {
            /* Pass predicate to choose_scan so it can try IndexScan */
            PhysicalPlan *scan = choose_scan(ctx, arena, lplan->left,
                                              lplan->filter.predicate);
            if (!scan) return NULL;

            /* If index scan was chosen, the filter is already "embedded" in the scan.
             * We still need a Filter node on top to actually apply the predicate
             * during execution (the IndexScan metadata is used for access path only). */
            PhysicalPlan *filt = pplan_new(arena, POP_FILTER);
            if (!filt) return scan;
            filt->filter.predicate = lplan->filter.predicate;
            filt->left = scan;

            CollectionStats *cs = stats_get(&ctx->stats,
                lplan->left->scan.collection_name);
            double sel = stats_estimate_selectivity(cs, lplan->filter.predicate);
            filt->estimated_rows  = scan->estimated_rows * sel;
            if (filt->estimated_rows < 1) filt->estimated_rows = 1;
            filt->estimated_cost  = scan->estimated_cost +
                cost_filter(m, scan->estimated_rows, sel);
            return filt;
        }
        /* Generic filter over any child */
        PhysicalPlan *child = enumerate_physical(ctx, arena, lplan->left);
        if (!child) return NULL;
        PhysicalPlan *filt = pplan_new(arena, POP_FILTER);
        if (!filt) return child;
        filt->filter.predicate = lplan->filter.predicate;
        filt->left = child;
        CollectionStats *cs = NULL;
        /* Try to find collection stats from child */
        if (lplan->left && lplan->left->type == LOP_SCAN)
            cs = stats_get(&ctx->stats, lplan->left->scan.collection_name);
        double sel = stats_estimate_selectivity(cs, lplan->filter.predicate);
        filt->estimated_rows = child->estimated_rows * sel;
        if (filt->estimated_rows < 1) filt->estimated_rows = 1;
        filt->estimated_cost = child->estimated_cost +
            cost_filter(m, child->estimated_rows, sel);
        return filt;
    }

    case LOP_JOIN: {
        PhysicalPlan *left  = enumerate_physical(ctx, arena, lplan->left);
        PhysicalPlan *right = enumerate_physical(ctx, arena, lplan->right);
        if (!left || !right) return left ? left : right;
        return build_physical_join(ctx, arena, lplan, left, right);
    }

    case LOP_SORT: {
        PhysicalPlan *child = enumerate_physical(ctx, arena, lplan->left);
        if (!child) return NULL;
        PhysicalPlan *sort = pplan_new(arena, POP_SORT);
        if (!sort) return child;
        sort->sort.fields   = lplan->sort.fields;
        sort->sort.n_fields = lplan->sort.n_fields;
        sort->left = child;
        sort->estimated_rows = child->estimated_rows;

        /* Determine avg row size from stats if possible */
        double avg_size = 128.0;
        sort->estimated_cost = child->estimated_cost +
            cost_sort(m, child->estimated_rows, avg_size);
        return sort;
    }

    case LOP_LIMIT: {
        PhysicalPlan *child = enumerate_physical(ctx, arena, lplan->left);
        if (!child) return NULL;
        PhysicalPlan *lim = pplan_new(arena, POP_LIMIT);
        if (!lim) return child;
        lim->limit.limit = lplan->limit.limit;
        lim->limit.skip  = lplan->limit.skip;
        lim->left = child;
        double rows = child->estimated_rows;
        int skip = lplan->limit.skip;
        int lim_n = lplan->limit.limit;
        if (skip > 0 && rows > skip) rows -= skip;
        if (lim_n >= 0 && rows > lim_n) rows = lim_n;
        lim->estimated_rows = rows < 0 ? 0 : rows;
        lim->estimated_cost = child->estimated_cost; /* LIMIT adds negligible cost */
        return lim;
    }

    case LOP_AGGREGATE: {
        PhysicalPlan *child = enumerate_physical(ctx, arena, lplan->left);
        if (!child) return NULL;
        double in_rows = child->estimated_rows;
        /* Estimate distinct groups: sqrt(rows) heuristic */
        double groups = in_rows > 0 ? sqrt(in_rows) : 1;

        /* Choose: HashAggregate vs StreamAggregate
         * Stream needs sorted input — only free if child is already sorted. */
        int child_sorted = (child->type == POP_SORT);
        double cost_ha = cost_hash_aggregate(m, in_rows, groups);
        double cost_sa = cost_stream_aggregate(m, in_rows, child_sorted);

        PhysicalOpType agg_type = POP_HASH_AGGREGATE;
        double agg_cost = cost_ha;
        if (child_sorted && cost_sa < cost_ha) {
            agg_type = POP_STREAM_AGGREGATE;
            agg_cost = cost_sa;
        }

        PhysicalPlan *agg = pplan_new(arena, agg_type);
        if (!agg) return child;
        strncpy(agg->aggregate.group_by_field, lplan->aggregate.group_by_field,
                sizeof(agg->aggregate.group_by_field) - 1);
        agg->aggregate.aggs   = lplan->aggregate.aggs;
        agg->aggregate.n_aggs = lplan->aggregate.n_aggs;
        agg->left = child;
        agg->estimated_rows  = groups;
        agg->estimated_cost  = child->estimated_cost + agg_cost;
        return agg;
    }

    case LOP_PROJECT: {
        /* Just pass through for now — project is transparent at execution */
        return enumerate_physical(ctx, arena, lplan->left);
    }

    default: {
        /* Unknown logical op — return seq scan fallback */
        PhysicalPlan *fallback = pplan_new(arena, POP_SEQ_SCAN);
        if (fallback) {
            fallback->estimated_cost = 9999;
            fallback->estimated_rows = 1000;
        }
        return fallback;
    }
    }
}

/* ===== Main optimizer entry point ===== */

PhysicalPlan* optimizer_run(OptimizerCtx *ctx, const Query *q, Arena *arena) {
    if (!ctx || !q || !arena) return NULL;

    /* Step 1: Build logical plan from AST */
    LogicalPlan *lplan = build_logical_plan(q, arena);
    if (!lplan) return NULL;

    if (ctx->trace) {
        printf("\n=== OPTIMIZER TRACE ===\n");
        printf("--- Logical Plan (initial) ---\n");
        logical_plan_print(lplan, 0);
    }

    if (ctx->mode == HUGO_OPT_OFF) {
        /* Should not reach here — caller checks mode before calling optimizer */
        return NULL;
    }

    /* Step 2: Apply logical rewrite rules (Phase 4 + Phase 10) */
    if (ctx->mode == HUGO_OPT_HEURISTIC || ctx->mode == HUGO_OPT_COST_BASED) {
        lplan = apply_all_rules(lplan, arena);
        /* Phase 10: advanced rules */
        lplan = apply_advanced_rules(lplan, arena);
        if (ctx->trace) {
            printf("--- Logical Plan (after rules) ---\n");
            logical_plan_print(lplan, 0);
        }
    }

    /* Step 3: Check for multi-table JOIN — use Phase 7 DP if applicable */
    PhysicalPlan *pplan = NULL;

    /* Detect if we have a JOIN in the logical plan and mode is cost-based */
    int has_join = 0;
    {
        LogicalPlan *cur = lplan;
        while (cur) {
            if (cur->type == LOP_JOIN) { has_join = 1; break; }
            cur = cur->left;
        }
    }

    if (has_join && ctx->mode == HUGO_OPT_COST_BASED && q->join) {
        /* Phase 7: Use System R DP for join order optimization */
        JoinOrderCtx jctx;
        join_order_ctx_init(&jctx, ctx->db, &ctx->stats, &ctx->cost_model,
                            arena, ctx->trace);

        /* Add left (local) table */
        join_order_add_table(&jctx, q->collection, q->haar);
        /* Add right (joined) table */
        join_order_add_table(&jctx, q->join->target_coll, NULL);
        /* Add join predicate */
        join_order_add_predicate(&jctx,
                                  q->collection,      q->join->local_field,
                                  q->join->target_coll, q->join->target_field);

        if (ctx->trace)
            printf("--- Phase 7: Join Order DP (%zu tables) ---\n",
                   jctx.n_tables);

        PhysicalPlan *join_plan = join_order_optimize(&jctx);
        if (join_plan) {
            /* Wrap with sort/limit if needed */
            pplan = join_plan;
            /* Add Sort if query has ORDER BY */
            if (q->orange_bi) {
                PhysicalPlan *sort = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
                if (sort) {
                    memset(sort, 0, sizeof(PhysicalPlan));
                    sort->type = POP_SORT;
                    sort->sort.fields = q->orange_bi;
                    sort->left = pplan;
                    sort->estimated_rows = pplan->estimated_rows;
                    sort->estimated_cost = pplan->estimated_cost +
                        cost_sort(&ctx->cost_model, pplan->estimated_rows, 128.0);
                    pplan = sort;
                }
            }
            /* Add Limit if query has LIMIT/SKIP */
            if (q->lime >= 0 || q->skopan > 0) {
                PhysicalPlan *lim = (PhysicalPlan*)arena_alloc(arena, sizeof(PhysicalPlan));
                if (lim) {
                    memset(lim, 0, sizeof(PhysicalPlan));
                    lim->type = POP_LIMIT;
                    lim->limit.limit = q->lime;
                    lim->limit.skip  = q->skopan;
                    lim->left = pplan;
                    double rows = pplan->estimated_rows;
                    if (q->skopan > 0 && rows > q->skopan) rows -= q->skopan;
                    if (q->lime >= 0 && rows > q->lime) rows = q->lime;
                    lim->estimated_rows = rows < 0 ? 0 : rows;
                    lim->estimated_cost = pplan->estimated_cost;
                    pplan = lim;
                }
            }
        }
    }

    /* Fallback: Phase 6 physical plan enumeration (non-join or heuristic) */
    if (!pplan)
        pplan = enumerate_physical(ctx, arena, lplan);
    if (!pplan) return NULL;

    if (ctx->trace) {
        printf("--- Physical Plan (chosen) ---\n");
        physical_plan_print(pplan, 0);
        printf("=== END OPTIMIZER TRACE ===\n\n");
    }

    return pplan;
}

/* ===== EXPLAIN formatter ===== */

static void explain_node(const PhysicalPlan *plan, int depth,
                          char *buf, size_t *off, size_t buf_size) {
    if (!plan || *off >= buf_size - 2) return;

    /* Indent */
    for (int i = 0; i < depth * 4 && *off < buf_size - 2; i++)
        buf[(*off)++] = ' ';
    if (depth > 0 && *off + 4 < buf_size) {
        buf[(*off)++] = '\xE2'; buf[(*off)++] = '\x94'; /* UTF-8 └ */
        buf[(*off)++] = '\x94'; buf[(*off)++] = ' ';
    }

    /* Node description */
    char node_desc[256];
    int nc = 0;
    switch (plan->type) {
    case POP_SEQ_SCAN:
        nc = snprintf(node_desc, sizeof(node_desc),
            "SeqScan %s (cost=%.2f rows=%.0f)",
            plan->seq_scan.collection_name,
            plan->estimated_cost, plan->estimated_rows);
        break;
    case POP_INDEX_SCAN:
        nc = snprintf(node_desc, sizeof(node_desc),
            "IndexScan %s.%s (cost=%.2f rows=%.0f)",
            plan->index_scan.collection_name, plan->index_scan.index_col,
            plan->estimated_cost, plan->estimated_rows);
        break;
    case POP_FILTER: {
        const Condition *c = plan->filter.predicate;
        if (c && c->type == COND_CMP) {
            const char *op = "op";
            switch (c->op) {
            case TOK_OP_BG: op = "="; break;
            case TOK_OP_KC: op = "!="; break;
            case TOK_OP_LH: op = "<"; break;
            case TOK_OP_BH: op = ">"; break;
            case TOK_OP_LHB: op = "<="; break;
            case TOK_OP_BHB: op = ">="; break;
            case TOK_OP_XAU: op = "contains"; break;
            default: break;
            }
            if (c->value.type == VAL_NUM)
                nc = snprintf(node_desc, sizeof(node_desc),
                    "Filter (%s %s %g) (cost=%.2f rows=%.0f)",
                    c->field, op, c->value.num,
                    plan->estimated_cost, plan->estimated_rows);
            else
                nc = snprintf(node_desc, sizeof(node_desc),
                    "Filter (%s %s '%s') (cost=%.2f rows=%.0f)",
                    c->field, op, c->value.str,
                    plan->estimated_cost, plan->estimated_rows);
        } else {
            nc = snprintf(node_desc, sizeof(node_desc),
                "Filter (...) (cost=%.2f rows=%.0f)",
                plan->estimated_cost, plan->estimated_rows);
        }
        break;
    }
    case POP_NESTED_LOOP_JOIN:
    case POP_HASH_JOIN:
    case POP_SORT_MERGE_JOIN:
        nc = snprintf(node_desc, sizeof(node_desc),
            "%s (%s = %s) (cost=%.2f rows=%.0f)",
            pop_type_name(plan->type),
            plan->join.left_col, plan->join.right_col,
            plan->estimated_cost, plan->estimated_rows);
        break;
    case POP_SORT:
        nc = snprintf(node_desc, sizeof(node_desc),
            "Sort (cost=%.2f rows=%.0f)",
            plan->estimated_cost, plan->estimated_rows);
        break;
    case POP_LIMIT:
        nc = snprintf(node_desc, sizeof(node_desc),
            "Limit %d skip=%d (cost=%.2f rows=%.0f)",
            plan->limit.limit, plan->limit.skip,
            plan->estimated_cost, plan->estimated_rows);
        break;
    case POP_HASH_AGGREGATE:
    case POP_STREAM_AGGREGATE:
        nc = snprintf(node_desc, sizeof(node_desc),
            "%s (group by %s) (cost=%.2f rows=%.0f)",
            pop_type_name(plan->type),
            plan->aggregate.group_by_field,
            plan->estimated_cost, plan->estimated_rows);
        break;
    default:
        nc = snprintf(node_desc, sizeof(node_desc), "%s", pop_type_name(plan->type));
        break;
    }
    (void)nc;

    /* Copy to output buffer */
    size_t len = strlen(node_desc);
    if (*off + len + 2 < buf_size) {
        memcpy(buf + *off, node_desc, len);
        *off += len;
        buf[(*off)++] = '\n';
    }

    explain_node(plan->left,  depth + 1, buf, off, buf_size);
    explain_node(plan->right, depth + 1, buf, off, buf_size);
}

int optimizer_explain(const PhysicalPlan *plan, char *buf, size_t buf_size) {
    if (!plan || !buf || buf_size == 0) return -1;
    memset(buf, 0, buf_size);
    size_t off = 0;
    const char *header = "Physical Plan:\n";
    size_t hlen = strlen(header);
    if (hlen < buf_size) {
        memcpy(buf, header, hlen);
        off = hlen;
    }
    explain_node(plan, 0, buf, &off, buf_size);
    /* Footer */
    char footer[128];
    snprintf(footer, sizeof(footer),
             "\nEstimated total cost: %.2f\nEstimated rows: %.0f\n",
             plan->estimated_cost, plan->estimated_rows);
    size_t flen = strlen(footer);
    if (off + flen < buf_size)
        memcpy(buf + off, footer, flen);
    return 0;
}
