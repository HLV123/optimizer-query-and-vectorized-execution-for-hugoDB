/* logical_plan.h — Logical Plan representation for Hugo DB Optimizer
 *
 * Logical plan = relational algebra tree. Mỗi node = 1 operator.
 * Root = final result. Không phụ thuộc vào physical algorithm.
 *
 * Phase 1: Build từ AST, pretty-print.
 * Phase 2+: Statistics, cost estimation, rule transformations.
 */
#ifndef HUGO_LOGICAL_PLAN_H
#define HUGO_LOGICAL_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include "../../query/ast.h"
#include "arena.h"

/* ===== Logical Operator Types ===== */
typedef enum {
    LOP_SCAN,       /* full collection scan */
    LOP_FILTER,     /* WHERE predicate filter */
    LOP_PROJECT,    /* SELECT columns subset */
    LOP_JOIN,       /* JOIN two inputs */
    LOP_AGGREGATE,  /* GROUP BY + aggregations */
    LOP_SORT,       /* ORDER BY */
    LOP_LIMIT,      /* LIMIT + SKIP */
} LogicalOpType;

/* ===== Aggregation function (for LOP_AGGREGATE) ===== */
typedef struct AggFunc {
    HugoTokenType func;        /* TOK_POU (COUNT), TOK_SEP (SUM), etc. */
    char          field[128];  /* input field */
    char          out_name[256]; /* output column name, e.g. "pou_age" */
} AggFunc;

/* ===== Logical Plan Node ===== */
typedef struct LogicalPlan {
    LogicalOpType type;

    /* Children */
    struct LogicalPlan *left;
    struct LogicalPlan *right;   /* NULL for unary ops */

    /* Output schema: columns this node produces */
    char   **output_columns;
    size_t   n_output_columns;

    /* Operator-specific data */
    union {
        struct {
            char collection_name[64];
        } scan;

        struct {
            Condition *predicate;   /* borrowed from AST, do NOT free */
        } filter;

        struct {
            char   **projected_columns;
            size_t   n_projected;
        } project;

        struct {
            char     left_col[128];
            char     right_col[128];
            int      is_left_join;   /* 0 = INNER */
        } join;

        struct {
            char     group_by_field[128];
            AggFunc *aggs;
            size_t   n_aggs;
        } aggregate;

        struct {
            SortField *fields;  /* borrowed from AST */
            size_t     n_fields;
        } sort;

        struct {
            int limit;  /* -1 = no limit */
            int skip;   /* 0  = no skip  */
        } limit;
    };

    /* Statistics (filled during optimization) */
    double estimated_rows;
    double estimated_bytes;
} LogicalPlan;

/* ===== API ===== */

/* Build a logical plan from a parsed AST query.
 * Uses arena for all allocations — free via arena_free().
 * Does NOT mutate or own the query/AST. */
LogicalPlan* build_logical_plan(const Query *q, Arena *arena);

/* Pretty-print plan as ASCII tree to stdout.
 * depth = 0 for root call. */
void logical_plan_print(const LogicalPlan *plan, int depth);

/* Return human-readable operator type name */
const char* lop_type_name(LogicalOpType t);

#endif
