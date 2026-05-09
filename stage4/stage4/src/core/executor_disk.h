/* executor_disk.h — Executor cho DiskDB (Phase 8.a integration) */
#ifndef HUGO_EXECUTOR_DISK_H
#define HUGO_EXECUTOR_DISK_H

#include "../query/ast.h"
#include "../query/executor.h"    /* for HugoResult + result_print */
#include "disk_db.h"

int hugo_execute_disk(DiskDB *db, const Query *q, HugoResult *r);

/* Result_print cho disk version: r->docs là heap-allocated clones, cần free. */
void result_print_disk(const HugoResult *r);
void result_free_disk (HugoResult *r);

#endif

/* ===== Stage 3: Optimizer-aware execute ===== */
#include "optimizer/optimizer.h"

/* Initialize the global optimizer context.
 * Call once after ddb_open(). */
void hugo_optimizer_init(DiskDB *db, const char *db_path, OptimizerMode mode);

/* Enable/disable optimizer trace output */
void hugo_optimizer_set_trace(int trace);

/* Get global optimizer context (NULL if not initialized) */
OptimizerCtx* hugo_optimizer_get(void);

/* Optimizer-aware execute:
 * Routes FUNDEN/GOMAIL through optimizer when mode != OFF.
 * Handles VERB_ANALYZE to rebuild statistics.
 * All other verbs fall through to hugo_execute_disk(). */
int hugo_execute_disk_opt(DiskDB *db, const Query *q, HugoResult *r);
