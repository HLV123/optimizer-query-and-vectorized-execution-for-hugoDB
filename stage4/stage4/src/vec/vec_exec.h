/* vec_exec.h — Vectorized executor (thay thế phys_executor.c)
 *
 * Pipeline: PhysicalPlan + DiskDB → scan → ColBatch → filter → agg/sort → HugoResult
 *
 * Không thay đổi API bên ngoài — vẫn nhận PhysicalPlan* và trả HugoResult*
 * như phys_exec_run(). Caller chỉ cần đổi include + call site.
 */
#ifndef HUGO_VEC_EXEC_H
#define HUGO_VEC_EXEC_H

#include "../core/optimizer/physical_plan.h"
#include "../core/disk_db.h"
#include "../query/executor.h"
#include "../core/optimizer/arena.h"

/* Drop-in replacement cho phys_exec_run().
 * Trả về 0 on success, -1 on error (chi tiết trong r->err_*). */
int vec_exec_run(DiskDB *db, const PhysicalPlan *plan,
                 HugoResult *r, Arena *arena);

#include "vec_scan_cache.h"

/* Access the global scan cache (for cache invalidation from write paths) */
ScanCache* vec_get_scan_cache(void);

#endif
