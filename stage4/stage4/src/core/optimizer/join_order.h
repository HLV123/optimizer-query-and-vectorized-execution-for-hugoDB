/* join_order.h — System R Join Order Optimization (Phase 7)
 *
 * Implements dynamic programming to find optimal join order for N-way joins.
 * Uses left-deep tree restriction for simplicity (O(2^N) subsets).
 *
 * Reference: "Access Path Selection in a Relational Database Management System"
 *            Selinger et al., 1979 (System R paper)
 *
 * TableSet: bitmap of table indices (uint32_t supports up to 32 tables)
 * DP table: dp[S] = best PhysicalPlan for joining the set of tables S
 */
#ifndef HUGO_JOIN_ORDER_H
#define HUGO_JOIN_ORDER_H

#include <stdint.h>
#include <stddef.h>
#include "physical_plan.h"
#include "statistics.h"
#include "cost_model.h"
#include "../../core/disk_db.h"

/* Bitmap representing a subset of tables */
typedef uint32_t TableSet;

#define MAX_JOIN_TABLES 16   /* 2^16 = 65536 subsets — plenty for Hugo DB */

/* ===== Join predicate info ===== */
typedef struct JoinPredicate {
    int    left_table_idx;   /* index into tables[] array */
    int    right_table_idx;
    char   left_col[128];
    char   right_col[128];
} JoinPredicate;

/* ===== DP table entry ===== */
typedef struct DPEntry {
    TableSet      tables;       /* which tables this plan covers */
    PhysicalPlan *plan;         /* best physical plan found */
    double        cost;         /* total cost of this plan */
    double        est_rows;     /* estimated output cardinality */
    int           valid;        /* 1 if entry exists */
} DPEntry;

/* ===== DP table (hash map over TableSet bitmap) ===== */
#define DP_TABLE_SIZE 4096   /* power of 2, must be >= 2^MAX_JOIN_TABLES */

typedef struct DPTable {
    DPEntry entries[DP_TABLE_SIZE];
} DPTable;

/* ===== Context for join order optimization ===== */
typedef struct JoinOrderCtx {
    DiskDB          *db;
    StatsStore      *stats;
    CostModel       *cost_model;
    Arena           *arena;

    /* Tables being joined */
    const char      *tables[MAX_JOIN_TABLES];
    size_t           n_tables;

    /* Filter predicates per table (for base scan cost) */
    const Condition *table_filters[MAX_JOIN_TABLES];

    /* Join predicates between tables */
    JoinPredicate    join_preds[MAX_JOIN_TABLES * MAX_JOIN_TABLES];
    int              n_join_preds;

    /* DP table */
    DPTable          dp;

    /* Trace output */
    int              trace;
} JoinOrderCtx;

/* ===== API ===== */

/* Initialize join order context */
void join_order_ctx_init(JoinOrderCtx *ctx, DiskDB *db, StatsStore *stats,
                          CostModel *model, Arena *arena, int trace);

/* Add a table to the join set with optional filter predicate */
int join_order_add_table(JoinOrderCtx *ctx, const char *table_name,
                          const Condition *filter);

/* Add a join predicate between two tables */
int join_order_add_predicate(JoinOrderCtx *ctx,
                              const char *left_table, const char *left_col,
                              const char *right_table, const char *right_col);

/* Run System R DP algorithm. Returns best physical plan for all tables joined.
 * Returns NULL if no valid plan found (e.g., disconnected join graph). */
PhysicalPlan* join_order_optimize(JoinOrderCtx *ctx);

/* ===== Helpers (exposed for testing) ===== */

/* Enumerate all non-empty proper subsets s1 of s (s2 = s \ s1).
 * Calls callback for each split where s1 < s2 (avoid duplicates). */
typedef void (*split_callback_fn)(TableSet s1, TableSet s2, void *user_ctx);
void enumerate_splits(TableSet s, split_callback_fn cb, void *user_ctx);

/* Count bits set in a TableSet */
int tableset_popcount(TableSet s);

/* Find join predicate connecting two table subsets (NULL if none) */
const JoinPredicate* find_join_pred(const JoinOrderCtx *ctx,
                                     TableSet s1, TableSet s2);

#endif
