/* rules.h — Logical rewrite rules for Hugo DB optimizer
 *
 * Rule-based transformations applied before physical enumeration.
 * Rules preserve semantics (same result, different structure).
 *
 * Order: eliminate_redundant → constant_fold → push_predicates → push_projections
 * Apply repeatedly until fixpoint (no more changes).
 */
#ifndef HUGO_OPTIMIZER_RULES_H
#define HUGO_OPTIMIZER_RULES_H

#include "logical_plan.h"

/* Apply all rules until fixpoint. Returns (possibly new) root. */
LogicalPlan* apply_all_rules(LogicalPlan *plan, Arena *arena);

/* Individual rules — each returns modified plan (may return same node if no change) */
LogicalPlan* rule_eliminate_redundant(LogicalPlan *plan, Arena *arena);
LogicalPlan* rule_predicate_pushdown(LogicalPlan *plan, Arena *arena);
LogicalPlan* rule_projection_pushdown(LogicalPlan *plan, Arena *arena);

#endif
