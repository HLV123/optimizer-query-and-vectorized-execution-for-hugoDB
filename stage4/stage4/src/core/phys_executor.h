/* phys_executor.h — Execute a PhysicalPlan against DiskDB
 *
 * This is the "bottom" of the optimizer pipeline: takes the PhysicalPlan
 * chosen by optimizer.c and executes it, filling a HugoResult.
 *
 * Design: reuses eval_condition / scan logic from executor_disk.c via
 * static helpers redefined here. In a larger refactor these would be
 * shared through a common internal header.
 */
#ifndef HUGO_PHYS_EXECUTOR_H
#define HUGO_PHYS_EXECUTOR_H

#include "optimizer/physical_plan.h"
#include "../core/disk_db.h"
#include "../query/executor.h"

/* Execute a physical plan, filling result r.
 * arena is used for intermediate allocations (hash tables etc.).
 * Returns 0 on success. */
int phys_exec_run(DiskDB *db, const PhysicalPlan *plan,
                  HugoResult *r, Arena *arena);

#endif
