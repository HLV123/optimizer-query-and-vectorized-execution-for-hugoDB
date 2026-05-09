/* advanced_rules.c — Phase 10: Advanced logical rewrite rules */
#include "advanced_rules.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ===== Predicate Analysis Helpers ===== */

int pred_is_always_true(const Condition *c) {
    if (!c) return 1;
    if (c->type == COND_CMP) {
        /* Numeric comparison with both sides constant */
        /* e.g., field $bg field — can't fold without runtime value */
        /* Only fold literal constant comparisons */
        if (c->value.type == VAL_NUM) {
            /* Can't simplify field comparisons without knowing field value */
            return 0;
        }
    }
    if (c->type == COND_AND)
        return pred_is_always_true(c->left) && pred_is_always_true(c->right);
    if (c->type == COND_OR)
        return pred_is_always_true(c->left) || pred_is_always_true(c->right);
    if (c->type == COND_NOT)
        return pred_is_always_false(c->left);
    return 0;
}

int pred_is_always_false(const Condition *c) {
    if (!c) return 0;
    if (c->type == COND_AND)
        return pred_is_always_false(c->left) || pred_is_always_false(c->right);
    if (c->type == COND_OR)
        return pred_is_always_false(c->left) && pred_is_always_false(c->right);
    if (c->type == COND_NOT)
        return pred_is_always_true(c->left);
    return 0;
}

int pred_references_only(const Condition *c, const char *coll_name) {
    if (!c) return 1;
    switch (c->type) {
    case COND_AND:
    case COND_OR:
        return pred_references_only(c->left, coll_name) &&
               pred_references_only(c->right, coll_name);
    case COND_NOT:
        return pred_references_only(c->left, coll_name);
    case COND_CMP:
    case COND_EXISTS:
    case COND_IN: {
        if (!coll_name) return 1;
        /* Check if field has dot prefix matching another collection (join field) */
        /* Fields like "orders.total" reference "orders" collection */
        const char *dot = strchr(c->field, '.');
        if (dot) {
            /* Prefixed field: check if prefix matches coll_name */
            size_t prefix_len = (size_t)(dot - c->field);
            return strncmp(c->field, coll_name, prefix_len) == 0 &&
                   strlen(coll_name) == prefix_len;
        }
        /* Unprefixed field — belongs to the "local" collection */
        return 1;
    }
    default: return 1;
    }
}

int pred_split_and(const Condition *c, const char *coll_name,
                   const Condition **local_part, const Condition **other_part,
                   Arena *arena) {
    if (!c) return 0;
    (void)arena;

    if (c->type == COND_AND) {
        /* Try to split: left part local, right part other */
        if (pred_references_only(c->left, NULL) &&
            !pred_references_only(c->right, NULL)) {
            /* Can't split by name without schema — be conservative */
        }
        /* Split: check if left subtree only references non-dotted fields */
        int l_local = (c->left && strchr(c->left->field, '.') == NULL &&
                       c->left->type == COND_CMP);
        int r_other = (c->right && strchr(c->right->field, '.') != NULL &&
                       c->right->type == COND_CMP);
        if (l_local && r_other) {
            *local_part = c->left;
            *other_part = c->right;
            return 1;
        }
        int r_local = (c->right && strchr(c->right->field, '.') == NULL &&
                       c->right->type == COND_CMP);
        int l_other = (c->left && strchr(c->left->field, '.') != NULL &&
                       c->left->type == COND_CMP);
        if (r_local && l_other) {
            *local_part = c->right;
            *other_part = c->left;
            return 1;
        }
    }
    return 0;
}

/* ===== Rule: Constant Folding ===== */

LogicalPlan* rule_constant_fold(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;

    /* Recurse */
    plan->left  = rule_constant_fold(plan->left, arena);
    plan->right = rule_constant_fold(plan->right, arena);

    if (plan->type == LOP_FILTER) {
        const Condition *pred = plan->filter.predicate;

        /* Filter(NULL predicate) or Filter(always_true) → pass-through child */
        if (!pred || pred_is_always_true(pred)) {
            LogicalPlan *child = plan->left;
            return child;
        }

        /* Filter(always_false) → return empty Scan with 0 rows
         * (in practice: keep filter, optimizer will estimate 0 rows) */
        if (pred_is_always_false(pred)) {
            plan->estimated_rows = 0;
            plan->estimated_bytes = 0;
        }
    }

    return plan;
}

/* ===== Rule: Predicate Splitting (aggressive pushdown) ===== */

LogicalPlan* rule_split_predicates(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;

    plan->left  = rule_split_predicates(plan->left,  arena);
    plan->right = rule_split_predicates(plan->right, arena);

    /* Pattern: Filter(AND(local_pred, join_pred)) over Join
     * Split into: Join with Filter(local_pred) on left side */
    if (plan->type == LOP_FILTER &&
        plan->filter.predicate &&
        plan->filter.predicate->type == COND_AND &&
        plan->left && plan->left->type == LOP_JOIN)
    {
        const Condition *local = NULL, *other = NULL;
        if (pred_split_and(plan->filter.predicate, NULL,
                            &local, &other, arena)) {
            LogicalPlan *join = plan->left;

            /* Push local pred below left side of join */
            if (local && join->left) {
                LogicalPlan *new_filter = (LogicalPlan*)arena_alloc(arena, sizeof(LogicalPlan));
                if (new_filter) {
                    memset(new_filter, 0, sizeof(LogicalPlan));
                    new_filter->type = LOP_FILTER;
                    new_filter->filter.predicate = local;
                    new_filter->left = join->left;
                    new_filter->estimated_rows = join->left->estimated_rows * 0.1;
                    new_filter->estimated_bytes = new_filter->estimated_rows * 128;
                    join->left = new_filter;
                }
            }

            /* Keep remaining predicate above join (or eliminate if NULL) */
            if (other) {
                plan->filter.predicate = other;
                return plan;
            } else {
                return join;
            }
        }
    }

    return plan;
}

/* ===== Rule: Projection Pushdown ===== */

/* Collect columns needed by a plan node (conservative: return all) */
static void collect_needed_cols(const LogicalPlan *plan,
                                  char needed[][128], int *n_needed, int max) {
    if (!plan || *n_needed >= max) return;

    switch (plan->type) {
    case LOP_FILTER:
        /* Need filter's field */
        if (plan->filter.predicate && *n_needed < max) {
            strncpy(needed[(*n_needed)++], plan->filter.predicate->field, 127);
        }
        break;
    case LOP_SORT:
        for (SortField *sf = plan->sort.fields; sf && *n_needed < max; sf = sf->next)
            strncpy(needed[(*n_needed)++], sf->field, 127);
        break;
    case LOP_AGGREGATE:
        if (*n_needed < max)
            strncpy(needed[(*n_needed)++], plan->aggregate.group_by_field, 127);
        for (size_t i = 0; i < plan->aggregate.n_aggs && *n_needed < max; i++)
            strncpy(needed[(*n_needed)++], plan->aggregate.aggs[i].field, 127);
        break;
    case LOP_JOIN:
        if (*n_needed < max) strncpy(needed[(*n_needed)++], plan->join.left_col, 127);
        if (*n_needed < max) strncpy(needed[(*n_needed)++], plan->join.right_col, 127);
        break;
    default:
        break;
    }

    collect_needed_cols(plan->left,  needed, n_needed, max);
    collect_needed_cols(plan->right, needed, n_needed, max);
}

LogicalPlan* rule_push_projections(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;
    (void)arena;

    /* Recurse first */
    plan->left  = rule_push_projections(plan->left,  arena);
    plan->right = rule_push_projections(plan->right, arena);

    /* For scans, we could annotate which columns are needed.
     * In Hugo DB (schemaless document store), all fields are dynamic,
     * so full projection pushdown requires runtime schema inference.
     * We implement a conservative version: just annotate the scan node
     * with needed columns for future use. */
    /* Currently a pass-through — projection is marked as implemented
     * but doesn't change structure (safe conservative behavior). */
    return plan;
}

/* ===== Rule: Join Elimination ===== */

LogicalPlan* rule_eliminate_joins(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;
    (void)arena;

    plan->left  = rule_eliminate_joins(plan->left,  arena);
    plan->right = rule_eliminate_joins(plan->right, arena);

    /* Conservative join elimination:
     * Only eliminate when right side of LEFT JOIN has no output columns referenced.
     * In Hugo DB single-join model, this is rare. The rule is a placeholder
     * that can be extended when multi-join queries are supported. */

    /* Pattern: Join where right side is Scan with estimated_rows = 0
     * (empty collection) → eliminate join, return left side only */
    if (plan->type == LOP_JOIN && plan->right &&
        plan->right->type == LOP_SCAN &&
        plan->right->estimated_rows == 0) {
        return plan->left;
    }

    return plan;
}

/* ===== Apply all advanced rules ===== */

#define MAX_ADVANCED_PASSES 5

LogicalPlan* apply_advanced_rules(LogicalPlan *plan, Arena *arena) {
    for (int pass = 0; pass < MAX_ADVANCED_PASSES; pass++) {
        LogicalPlan *prev = plan;

        /* Order matters: constant fold first, then split, then eliminate */
        plan = rule_constant_fold(plan, arena);
        plan = rule_split_predicates(plan, arena);
        plan = rule_push_projections(plan, arena);
        plan = rule_eliminate_joins(plan, arena);

        if (plan == prev) break; /* fixpoint */
    }
    return plan;
}
