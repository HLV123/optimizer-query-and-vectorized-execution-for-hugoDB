/* advanced_rules.h — Phase 10: Advanced Logical Rewrite Rules
 *
 * Rules implemented:
 *   1. Constant folding: simplify constant expressions in predicates
 *   2. Projection pushdown: push column selection closer to scans
 *   3. Join elimination: remove joins where results aren't used
 *   4. Dead filter elimination: remove filters that always pass/fail
 */
#ifndef HUGO_ADVANCED_RULES_H
#define HUGO_ADVANCED_RULES_H

#include "logical_plan.h"

/* ===== Rule: Constant Folding =====
 * Simplifies predicates with constant values:
 *   Filter(true)  → eliminate filter (pass-through)
 *   Filter(false) → return empty (plan never produces rows)
 *   age + 0 = 25  → age = 25
 *   1 = 1         → true (eliminate)
 */
LogicalPlan* rule_constant_fold(LogicalPlan *plan, Arena *arena);

/* ===== Rule: Projection Pushdown =====
 * Adds explicit project nodes near scans to limit columns read.
 * In document DBs this is less impactful but reduces memory in joins.
 * Only projects columns actually needed by parent operators.
 */
LogicalPlan* rule_push_projections(LogicalPlan *plan, Arena *arena);

/* ===== Rule: Join Elimination =====
 * Removes joins when:
 *   - JOIN result columns are never referenced by parent
 *   - LEFT JOIN where right side always matches (FK constraint metadata)
 * Conservative: only eliminate when provably safe.
 */
LogicalPlan* rule_eliminate_joins(LogicalPlan *plan, Arena *arena);

/* ===== Rule: Predicate Splitting =====
 * Splits AND predicates and pushes each part as far down as possible.
 * More aggressive version of rule_predicate_pushdown from rules.c.
 * Example:
 *   Filter(a.age=25 AND b.total>100) over Join(a,b)
 *   → Join(Filter(a.age=25,a), Filter(b.total>100,b))
 */
LogicalPlan* rule_split_predicates(LogicalPlan *plan, Arena *arena);

/* Apply all advanced rules in correct order until fixpoint */
LogicalPlan* apply_advanced_rules(LogicalPlan *plan, Arena *arena);

/* ===== Predicate analysis helpers ===== */

/* Check if a condition always evaluates to true (constant true) */
int pred_is_always_true(const Condition *c);

/* Check if a condition always evaluates to false (constant false) */
int pred_is_always_false(const Condition *c);

/* Check if condition references only fields from one table/scan.
 * coll_name: collection name to check against (NULL = any single table).
 * Returns 1 if all field references match. */
int pred_references_only(const Condition *c, const char *coll_name);

/* Split AND condition into two parts: left_part (refs coll_name) and right_part (rest).
 * Returns 1 if split was possible. */
int pred_split_and(const Condition *c, const char *coll_name,
                   const Condition **left_part, const Condition **right_part,
                   Arena *arena);

#endif
