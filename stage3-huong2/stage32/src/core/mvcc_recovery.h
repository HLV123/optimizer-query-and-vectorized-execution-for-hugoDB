/* mvcc_recovery.h — MVCC WAL Recovery (Phase 5)
 *
 * Sau crash, recovery phải:
 *   1. Scan WAL để rebuild tx state map:
 *        tx_id → {begin_ts, commit_ts, ACTIVE|COMMITTED|ABORTED}
 *   2. Xác định "loser" transactions (BEGIN nhưng không COMMIT/ABORT)
 *      → mark ABORTED trong committed_table
 *   3. Rebuild version chain pointers từ WAL_MVCC_VERSION records:
 *        (coll_name, doc_id) → latest version_ptr
 *      → update doc_page_ids trong DiskColl
 *   4. Với aborted/loser versions: restore doc_page_ids về prev_version_ptr
 *      (undo append-only writes)
 *   5. Advance TsOracle vượt qua max commit_ts đã thấy trong WAL
 *      → đảm bảo timestamps không reuse sau restart
 *
 * Gọi sau wal_recover() thông thường (ARIES REDO/UNDO đã xử lý physical pages).
 * mvcc_recover() xử lý logical MVCC state trên top of recovered pages.
 */
#ifndef HUGO_MVCC_RECOVERY_H
#define HUGO_MVCC_RECOVERY_H

#include "disk_db.h"
#include "wal.h"

/* Chạy MVCC recovery sau khi wal_recover() đã chạy xong.
 * Trả về 0 OK, -1 lỗi. */
int mvcc_recover(DiskDB *db);

#endif
