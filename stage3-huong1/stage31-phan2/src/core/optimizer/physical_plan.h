/* physical_plan.h — Physical Plan representation
 *
 * Physical plan = concrete algorithm choices for each logical operator.
 * Iterator model: open() → next() → close() for pipeline execution.
 *
 * Phase 5: physical plan structs.
 * Phase 6: physical plan selection (cost-based enumeration).
 */
#ifndef HUGO_PHYSICAL_PLAN_H
#define HUGO_PHYSICAL_PLAN_H

#include <stddef.h>
#include "../../query/ast.h"
#include "logical_plan.h"
#include "arena.h"
#include "cost_model.h"

/* ===== Physical Operator Types ===== */
typedef enum {
    POP_SEQ_SCAN,           /* full table scan */
    POP_INDEX_SCAN,         /* scan using index */
    POP_FILTER,             /* apply predicate */
    POP_PROJECT,            /* column projection */
    POP_NESTED_LOOP_JOIN,   /* O(n*m) join */
    POP_HASH_JOIN,          /* O(n+m) join */
    POP_SORT_MERGE_JOIN,    /* O(n log n + m log m) join */
    POP_HASH_AGGREGATE,     /* hash-based GROUP BY */
    POP_STREAM_AGGREGATE,   /* streaming GROUP BY (sorted input) */
    POP_SORT,               /* external/in-memory sort */
    POP_LIMIT,              /* LIMIT + SKIP */
} PhysicalOpType;

/* ===== Physical Plan Node ===== */
typedef struct PhysicalPlan {
    PhysicalOpType type;

    /* Children */
    struct PhysicalPlan *left;
    struct PhysicalPlan *right;

    /* Cost (estimated by optimizer) */
    double estimated_cost;
    double estimated_rows;

    /* Operator-specific parameters */
    union {
        struct {
            char collection_name[64];
            int  use_buffer_pool;
        } seq_scan;

        struct {
            char  collection_name[64];
            char  index_col[128];
            Value low_val;
            Value high_val;
            int   has_low;
            int   has_high;
            int   asc;
        } index_scan;

        struct {
            Condition *predicate;   /* borrowed */
        } filter;

        struct {
            char   **columns;       /* in arena */
            size_t   n_columns;
        } project;

        struct {
            char left_col[128];
            char right_col[128];
        } join;

        struct {
            char     group_by_field[128];
            AggFunc *aggs;          /* borrowed from logical plan */
            size_t   n_aggs;
        } aggregate;

        struct {
            SortField *fields;      /* borrowed */
            size_t     n_fields;
        } sort;

        struct {
            int limit;
            int skip;
        } limit;
    };
} PhysicalPlan;

/* ===== API ===== */

const char* pop_type_name(PhysicalOpType t);

/* Pretty-print physical plan */
void physical_plan_print(const PhysicalPlan *plan, int depth);

/* Estimate total cost of a subtree */
double physical_plan_total_cost(const PhysicalPlan *plan);

#endif
