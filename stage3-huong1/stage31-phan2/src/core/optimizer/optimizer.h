/* optimizer.h — Main optimizer API for Hugo DB
 *
 * Pipeline:
 *   AST → build_logical_plan → apply_all_rules → enumerate_physical →
 *   cost each → select minimum → PhysicalPlan
 *
 * Usage:
 *   OptimizerCtx ctx;
 *   optimizer_ctx_init(&ctx, db, db_path);
 *   PhysicalPlan *plan = optimizer_run(&ctx, query, arena);
 *   // execute plan...
 *   optimizer_ctx_free(&ctx);
 */
#ifndef HUGO_OPTIMIZER_H
#define HUGO_OPTIMIZER_H

#include "logical_plan.h"
#include "physical_plan.h"
#include "statistics.h"
#include "cost_model.h"
#include "rules.h"
#include "../../core/disk_db.h"

/* ===== Optimizer mode ===== */
typedef enum {
    HUGO_OPT_OFF,          /* legacy direct AST execution */
    HUGO_OPT_HEURISTIC,    /* rule-based only (no cost estimation) */
    HUGO_OPT_COST_BASED,   /* full cost-based (default) */
} OptimizerMode;

/* ===== Optimizer context (one per DiskDB) ===== */
typedef struct OptimizerCtx {
    DiskDB       *db;
    StatsStore    stats;
    CostModel     cost_model;
    OptimizerMode mode;
    int           trace;   /* 1 = print debug trace */
} OptimizerCtx;

/* Initialize optimizer context.
 * db_path: used to derive stats file path. */
void optimizer_ctx_init(OptimizerCtx *ctx, DiskDB *db, const char *db_path);
void optimizer_ctx_free(OptimizerCtx *ctx);

/* Set optimizer mode at runtime */
void optimizer_set_mode(OptimizerCtx *ctx, OptimizerMode mode);

/* Enable/disable trace output */
void optimizer_set_trace(OptimizerCtx *ctx, int trace);

/* Main optimization entry point.
 * Returns physical plan allocated in arena (caller owns arena).
 * Returns NULL on error (falls back to legacy executor). */
PhysicalPlan* optimizer_run(OptimizerCtx *ctx, const Query *q, Arena *arena);

/* Run ANALYZE on a collection — rebuild statistics */
int optimizer_analyze(OptimizerCtx *ctx, const char *collection);

/* Translate PhysicalPlan to EXPLAIN string (written to buf) */
int optimizer_explain(const PhysicalPlan *plan, char *buf, size_t buf_size);

#endif

/* Stage 3 Phase 7+: join order DP and advanced rules */
#include "join_order.h"
#include "advanced_rules.h"
