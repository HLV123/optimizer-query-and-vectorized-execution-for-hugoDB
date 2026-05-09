/* rules.c — Logical rewrite rule implementations */
#include "rules.h"
#include <string.h>
#include <stdlib.h>

/* ===== Rule: Eliminate Redundant Operators ===== */

/* Limit(-1, skip=0) → no-op: pass through child */
/* Sort with no fields → no-op */
/* Project(all) → no-op (hard to know without schema, skip for now) */

LogicalPlan* rule_eliminate_redundant(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;
    (void)arena;

    /* Recurse first */
    plan->left  = rule_eliminate_redundant(plan->left,  arena);
    plan->right = rule_eliminate_redundant(plan->right, arena);

    switch (plan->type) {
    case LOP_LIMIT:
        /* Limit(-1, skip=0) is a no-op */
        if (plan->limit.limit < 0 && plan->limit.skip == 0)
            return plan->left;
        break;
    case LOP_SORT:
        /* Sort with zero fields is a no-op */
        if (plan->sort.n_fields == 0)
            return plan->left;
        break;
    case LOP_FILTER:
        /* NULL predicate filter is a no-op */
        if (!plan->filter.predicate)
            return plan->left;
        break;
    default:
        break;
    }
    return plan;
}

/* ===== Predicate analysis helpers ===== */

/* Check if a condition references ONLY fields from a given collection.
 * Since Hugo DB is schemaless (document DB), we can't statically determine
 * which fields belong to which collection. We use a simple heuristic:
 * if a JOIN is present, predicates on the "local" scan are pushed down.
 * For now: all unary predicates (COND_CMP, COND_EXISTS, COND_IN) can be
 * pushed below scans. AND-split predicates are handled below. */

/* Check if condition references any dot-prefixed fields (alias.field = join result) */
static int cond_has_alias_field(const Condition *c, const char *alias) {
    if (!c) return 0;
    switch (c->type) {
    case COND_AND:
    case COND_OR:
        return cond_has_alias_field(c->left, alias) ||
               cond_has_alias_field(c->right, alias);
    case COND_NOT:
        return cond_has_alias_field(c->left, alias);
    case COND_CMP:
    case COND_EXISTS:
    case COND_IN: {
        /* Check if field starts with alias. */
        size_t alen = strlen(alias);
        if (strncmp(c->field, alias, alen) == 0 && c->field[alen] == '.')
            return 1;
        return 0;
    }
    default: return 0;
    }
}

/* ===== Rule: Predicate Pushdown =====
 *
 * Push Filter nodes below Join nodes when predicate only references
 * one side of the join. This reduces rows before expensive join.
 *
 * Pattern:
 *   Filter(pred)
 *     └─ Join(lc = rc)
 *         ├─ left_child
 *         └─ right_child
 *
 * → if pred only refs left:
 *   Join(lc = rc)
 *     ├─ Filter(pred)
 *     │   └─ left_child
 *     └─ right_child
 */
LogicalPlan* rule_predicate_pushdown(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;

    /* Recurse children first */
    plan->left  = rule_predicate_pushdown(plan->left,  arena);
    plan->right = rule_predicate_pushdown(plan->right, arena);

    /* Pattern: Filter over Join */
    if (plan->type == LOP_FILTER &&
        plan->left  != NULL &&
        plan->left->type == LOP_JOIN &&
        plan->filter.predicate != NULL)
    {
        LogicalPlan *join  = plan->left;
        Condition   *pred  = plan->filter.predicate;

        /* Get the join alias / right-side collection name */
        const char *right_coll = join->right ? join->right->scan.collection_name : "";

        /* If predicate doesn't reference right-side alias at all,
         * push it below the left side of join */
        if (!cond_has_alias_field(pred, right_coll)) {
            /* Create new Filter under join->left */
            LogicalPlan *new_filter = (LogicalPlan*)arena_alloc(arena, sizeof(LogicalPlan));
            if (new_filter) {
                *new_filter = *plan;                    /* copy filter node */
                new_filter->left = join->left;          /* filter wraps left scan */
                new_filter->estimated_rows =
                    join->left->estimated_rows * 0.1;   /* rough estimate */
                join->left = new_filter;

                /* The outer Filter node is now redundant — return the join */
                return join;
            }
        }
    }

    return plan;
}

/* ===== Rule: Projection Pushdown =====
 *
 * Currently a no-op placeholder.
 * Full implementation would track needed columns per node and
 * add PROJECT nodes below Scans to avoid reading unneeded fields.
 * In a document DB with schemaless documents this has less impact,
 * so we defer to a stretch goal.
 */
LogicalPlan* rule_projection_pushdown(LogicalPlan *plan, Arena *arena) {
    if (!plan) return plan;
    (void)arena;
    plan->left  = rule_projection_pushdown(plan->left,  arena);
    plan->right = rule_projection_pushdown(plan->right, arena);
    return plan;
}

/* ===== Apply all rules until fixpoint ===== */

#define MAX_RULE_PASSES 10

LogicalPlan* apply_all_rules(LogicalPlan *plan, Arena *arena) {
    for (int pass = 0; pass < MAX_RULE_PASSES; pass++) {
        LogicalPlan *prev = plan;
        plan = rule_eliminate_redundant(plan, arena);
        plan = rule_predicate_pushdown(plan, arena);
        plan = rule_projection_pushdown(plan, arena);
        /* Fixpoint: if pointer unchanged (same tree shape), stop */
        if (plan == prev) break;
    }
    return plan;
}
