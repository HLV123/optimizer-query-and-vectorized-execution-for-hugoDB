/* logical_plan.c — Build logical plan from HugoQL AST */
#include "logical_plan.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===== Helper: make a new node in arena ===== */
static LogicalPlan* lplan_node(Arena *arena, LogicalOpType type) {
    LogicalPlan *p = (LogicalPlan*)arena_alloc(arena, sizeof(LogicalPlan));
    if (!p) return NULL;
    memset(p, 0, sizeof(LogicalPlan));
    p->type            = type;
    p->estimated_rows  = 1000.0; /* default fallback */
    p->estimated_bytes = 1000.0 * 128; /* ~128 bytes/row default */
    return p;
}

/* ===== Build Scan node ===== */
static LogicalPlan* build_scan(const Query *q, Arena *arena) {
    LogicalPlan *scan = lplan_node(arena, LOP_SCAN);
    if (!scan) return NULL;
    strncpy(scan->scan.collection_name, q->collection,
            sizeof(scan->scan.collection_name) - 1);
    return scan;
}

/* ===== Build Sort node (wraps child) ===== */
static LogicalPlan* build_sort(const Query *q, LogicalPlan *child, Arena *arena) {
    if (!q->orange_bi) return child;
    LogicalPlan *sort = lplan_node(arena, LOP_SORT);
    if (!sort) return child;
    sort->sort.fields   = q->orange_bi; /* borrowed */
    sort->sort.n_fields = 0;
    for (SortField *sf = q->orange_bi; sf; sf = sf->next)
        sort->sort.n_fields++;
    sort->left = child;
    sort->estimated_rows = child->estimated_rows;
    sort->estimated_bytes = child->estimated_bytes;
    return sort;
}

/* ===== Build Limit node (wraps child) ===== */
static LogicalPlan* build_limit(const Query *q, LogicalPlan *child, Arena *arena) {
    if (q->lime < 0 && q->skopan == 0) return child;
    LogicalPlan *lim = lplan_node(arena, LOP_LIMIT);
    if (!lim) return child;
    lim->limit.limit = q->lime;
    lim->limit.skip  = q->skopan;
    lim->left = child;
    double rows = child->estimated_rows;
    if (q->skopan > 0 && rows > q->skopan) rows -= q->skopan;
    if (q->lime >= 0 && rows > q->lime)    rows  = q->lime;
    lim->estimated_rows  = rows < 0 ? 0 : rows;
    lim->estimated_bytes = lim->estimated_rows * 128;
    return lim;
}

/* ===== Build Filter node (wraps child) ===== */
static LogicalPlan* build_filter(const Query *q, LogicalPlan *child, Arena *arena) {
    if (!q->haar) return child;
    LogicalPlan *filt = lplan_node(arena, LOP_FILTER);
    if (!filt) return child;
    filt->filter.predicate = q->haar; /* borrowed */
    filt->left = child;
    /* Default selectivity = 10% unless statistics override later */
    filt->estimated_rows  = child->estimated_rows * 0.1;
    filt->estimated_bytes = filt->estimated_rows * 128;
    return filt;
}

/* ===== Build Aggregate node (wraps child) ===== */
static LogicalPlan* build_aggregate(const Query *q, LogicalPlan *child, Arena *arena) {
    if (!q->gremb_bi) return child;
    LogicalPlan *agg = lplan_node(arena, LOP_AGGREGATE);
    if (!agg) return child;
    strncpy(agg->aggregate.group_by_field, q->gremb_bi->field,
            sizeof(agg->aggregate.group_by_field) - 1);

    /* Build AggFunc array */
    int n = q->gremb_bi->n_aggs;
    if (n > 0) {
        agg->aggregate.aggs = (AggFunc*)arena_alloc(arena, sizeof(AggFunc) * n);
        if (agg->aggregate.aggs) {
            for (int i = 0; i < n; i++) {
                agg->aggregate.aggs[i].func = q->gremb_bi->agg_funcs[i];
                strncpy(agg->aggregate.aggs[i].field,
                        q->gremb_bi->agg_fields[i],
                        sizeof(agg->aggregate.aggs[i].field) - 1);
                /* Generate output name: "pou_age", "sep_salary" etc. */
                const char *fn = "agg";
                switch (q->gremb_bi->agg_funcs[i]) {
                case TOK_POU: fn = "pou"; break;
                case TOK_SEP: fn = "sep"; break;
                case TOK_AWR: fn = "awr"; break;
                case TOK_MIE: fn = "mie"; break;
                case TOK_MAF: fn = "maf"; break;
                default: break;
                }
                snprintf(agg->aggregate.aggs[i].out_name,
                         sizeof(agg->aggregate.aggs[i].out_name),
                         "%s_%s", fn, q->gremb_bi->agg_fields[i]);
            }
        }
    }
    agg->aggregate.n_aggs = (size_t)n;
    agg->left = child;
    /* Estimate: distinct values in group_by field — default sqrt(rows) */
    double in_rows = child->estimated_rows;
    double groups  = in_rows > 0 ? (in_rows < 100 ? in_rows : 100) : 1;
    agg->estimated_rows  = groups;
    agg->estimated_bytes = groups * 256;
    return agg;
}

/* ===== Build Join node ===== */
static LogicalPlan* build_join(const Query *q, LogicalPlan *left_plan, Arena *arena) {
    if (!q->join) return left_plan;
    LogicalPlan *join = lplan_node(arena, LOP_JOIN);
    if (!join) return left_plan;

    strncpy(join->join.left_col,  q->join->local_field,
            sizeof(join->join.left_col) - 1);
    strncpy(join->join.right_col, q->join->target_field,
            sizeof(join->join.right_col) - 1);
    join->join.is_left_join = 0; /* INNER */

    /* Build right scan for target collection */
    LogicalPlan *right_scan = lplan_node(arena, LOP_SCAN);
    if (!right_scan) { return left_plan; }
    strncpy(right_scan->scan.collection_name, q->join->target_coll,
            sizeof(right_scan->scan.collection_name) - 1);

    join->left  = left_plan;
    join->right = right_scan;
    /* Estimate join output: min(left, right) * 10% */
    double l = left_plan->estimated_rows;
    double r = right_scan->estimated_rows;
    join->estimated_rows  = (l < r ? l : r) * 0.1;
    join->estimated_bytes = join->estimated_rows * 256;
    return join;
}

/* ===== Main entry: build_logical_plan ===== */
LogicalPlan* build_logical_plan(const Query *q, Arena *arena) {
    if (!q || !arena) return NULL;

    /* Only FUNDEN (SELECT) and GOMAIL (GROUP BY aggregate) map to
     * a rich logical plan. Other verbs (INSERT, UPDATE, DELETE, DDL)
     * are single-node plans for now — optimizer passes them through. */

    switch (q->verb) {
    case VERB_FUNDEN: {
        /* Bottom-up construction: Scan → Filter → [Join] → [Sort] → [Limit] */
        LogicalPlan *plan = build_scan(q, arena);
        if (!plan) return NULL;

        /* Filter (WHERE haar) */
        plan = build_filter(q, plan, arena);

        /* JOIN (if present) — wrap around filter result */
        plan = build_join(q, plan, arena);

        /* Sort (ORDER BY) */
        plan = build_sort(q, plan, arena);

        /* Limit / Skip */
        plan = build_limit(q, plan, arena);

        return plan;
    }

    case VERB_GOMAIL: {
        /* GOMAIL = GROUP BY aggregate query */
        LogicalPlan *plan = build_scan(q, arena);
        if (!plan) return NULL;

        /* Filter before aggregate (predicate pushdown already done here) */
        plan = build_filter(q, plan, arena);

        /* Aggregate */
        plan = build_aggregate(q, plan, arena);

        /* Sort result if needed (GOMAIL doesn't have ORDER BY in current AST,
         * but we handle it defensively) */
        plan = build_sort(q, plan, arena);
        plan = build_limit(q, plan, arena);
        return plan;
    }

    default: {
        /* For INSERT, UPDATE, DELETE, DDL: return a minimal SCAN node
         * so the optimizer pipeline doesn't break.
         * Execution falls through to legacy executor for these. */
        return build_scan(q, arena);
    }
    }
}

/* ===== Pretty print ===== */
const char* lop_type_name(LogicalOpType t) {
    switch (t) {
    case LOP_SCAN:      return "Scan";
    case LOP_FILTER:    return "Filter";
    case LOP_PROJECT:   return "Project";
    case LOP_JOIN:      return "Join";
    case LOP_AGGREGATE: return "Aggregate";
    case LOP_SORT:      return "Sort";
    case LOP_LIMIT:     return "Limit";
    default:            return "Unknown";
    }
}

static void print_indent(int depth, int is_right) {
    for (int i = 0; i < depth - 1; i++) printf("    ");
    if (depth > 0) printf(is_right ? "    └─ " : "    ├─ ");
}

void logical_plan_print(const LogicalPlan *plan, int depth) {
    if (!plan) return;

    /* Print this node */
    if (depth == 0) printf("LogicalPlan:\n");
    print_indent(depth, 1);

    switch (plan->type) {
    case LOP_SCAN:
        printf("Scan(%s)", plan->scan.collection_name);
        break;
    case LOP_FILTER: {
        /* Simple condition description */
        const Condition *c = plan->filter.predicate;
        if (c && c->type == COND_CMP) {
            const char *op = "?";
            switch (c->op) {
            case TOK_OP_BG:  op = "=";       break;
            case TOK_OP_KC:  op = "!=";      break;
            case TOK_OP_LH:  op = "<";       break;
            case TOK_OP_BH:  op = ">";       break;
            case TOK_OP_LHB: op = "<=";      break;
            case TOK_OP_BHB: op = ">=";      break;
            case TOK_OP_XAU: op = "contains"; break;
            default: op = "op"; break;
            }
            if (c->value.type == VAL_NUM)
                printf("Filter(%s %s %g)", c->field, op, c->value.num);
            else
                printf("Filter(%s %s '%s')", c->field, op, c->value.str);
        } else if (c && c->type == COND_EXISTS) {
            printf("Filter(EXISTS %s)", c->field);
        } else if (c && c->type == COND_AND) {
            printf("Filter(AND)");
        } else if (c && c->type == COND_OR) {
            printf("Filter(OR)");
        } else {
            printf("Filter(...)");
        }
        break;
    }
    case LOP_PROJECT:
        printf("Project(%zu cols)", plan->project.n_projected);
        break;
    case LOP_JOIN:
        printf("Join(%s = %s)", plan->join.left_col, plan->join.right_col);
        break;
    case LOP_AGGREGATE:
        printf("Aggregate(group by %s, %zu aggs)",
               plan->aggregate.group_by_field, plan->aggregate.n_aggs);
        break;
    case LOP_SORT: {
        printf("Sort(");
        int first = 1;
        for (SortField *sf = plan->sort.fields; sf; sf = sf->next) {
            if (!first) printf(", ");
            printf("%s %s", sf->field, sf->descending ? "DESC" : "ASC");
            first = 0;
        }
        printf(")");
        break;
    }
    case LOP_LIMIT:
        if (plan->limit.skip > 0)
            printf("Limit(%d skip=%d)", plan->limit.limit, plan->limit.skip);
        else
            printf("Limit(%d)", plan->limit.limit);
        break;
    default:
        printf("Unknown");
    }

    printf("  [est_rows=%.0f]", plan->estimated_rows);
    printf("\n");

    /* Children */
    if (plan->left && plan->right) {
        /* Binary node: print left first, then right */
        logical_plan_print(plan->left,  depth + 1);
        logical_plan_print(plan->right, depth + 1);
    } else if (plan->left) {
        logical_plan_print(plan->left, depth + 1);
    }
}
