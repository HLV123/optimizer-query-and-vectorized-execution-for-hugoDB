/* mvcc_vacuum.h — MVCC Garbage Collection (Phase 6)
 *
 * Vacuum reclaims old DocVersion entries không còn visible với bất kỳ
 * active transaction nào.
 *
 * Algorithm:
 *   1. Tính oldest_visible_ts = min(begin_ts) của tất cả active transactions.
 *      Nếu không có active tx: oldest_visible_ts = current ts.
 *   2. Với mỗi (collection, doc_id) trong DB:
 *      Walk version chain từ latest.
 *      Tìm version V đầu tiên có created_ts <= oldest_visible_ts (visible tới mọi tx).
 *      Mọi version OLDer hơn V trong chain → "safe to remove".
 *   3. "Remove" (Phase 6 simple): đánh dấu prev_version_ptr = VERSION_PTR_NULL
 *      trên V (cắt đuôi chain). Page cũ được đánh dấu PAGE_TYPE_FREE để reuse.
 *
 * Vacuum modes:
 *   MVCC_VACUUM_SIMPLE  — cắt chain, không compact pages (fast, waste space)
 *   MVCC_VACUUM_COMPACT — reserved cho Phase 6b (không implement lần này)
 *
 * Statistics:
 *   Trả về số versions đã remove và pages freed.
 */
#ifndef HUGO_MVCC_VACUUM_H
#define HUGO_MVCC_VACUUM_H

#include "disk_db.h"

typedef struct {
    uint64_t versions_removed;  /* số DocVersion entries đã xóa */
    uint64_t pages_freed;       /* số pages đánh dấu free */
    uint64_t oldest_visible_ts; /* timestamp đã dùng cho GC */
} VacuumStats;

/* Chạy foreground vacuum trên toàn DB.
 * Trả về 0 OK, -1 lỗi. */
int mvcc_vacuum(DiskDB *db, VacuumStats *stats_out);

/* Tính oldest_visible_ts từ active transaction registry.
 * Nếu không có active tx: trả về ts_oracle_current(). */
uint64_t mvcc_oldest_visible_ts(DiskDB *db);

#endif
