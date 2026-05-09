/* mvcc_read.h — MVCC Read Path + Visibility Check
 *
 * Implements snapshot isolation visibility algorithm:
 *
 *   Một version V visible với transaction T nếu:
 *   1. V không được tạo bởi aborted tx
 *   2. V được tạo bởi chính T (thấy own writes)
 *      → nhưng nếu T cũng delete V thì không visible
 *   3. V không được tạo bởi tx trong active_set của T
 *      (những tx đang in-flight tại thời điểm T begin)
 *   4. V được tạo trước hoặc tại begin_ts của T
 *      (V.created_ts <= T.begin_ts)
 *   5. V không bị delete hoặc delete bởi tx không visible với T
 *
 *   Algorithm: walk version chain từ latest → oldest, return version đầu tiên
 *   satisfy visibility rules. Nếu không có → document not found.
 *
 * API chính:
 *   mvcc_find_doc() — tìm document visible với tx trong collection
 *   mvcc_scan()     — scan toàn collection, trả về docs visible với tx
 */
#ifndef HUGO_MVCC_READ_H
#define HUGO_MVCC_READ_H

#include "disk_db.h"
#include "mvcc_tx.h"
#include "doc_version.h"
#include "../query/ast.h"

/* ===== Visibility check ===== */

/* Trạng thái visibility của 1 version với 1 transaction */
typedef enum {
    VIS_VISIBLE   = 0,   /* version visible → đây là version cần return */
    VIS_SKIP      = 1,   /* version không visible → walk sang prev */
    VIS_NOT_FOUND = 2,   /* document đã bị delete theo visibility rules */
    VIS_ERROR     = 3,   /* lỗi đọc data */
} VisibilityResult;

/* Kiểm tra state của tx_id từ registry + committed table.
 * Trả về: 0 = committed, 1 = active, 2 = aborted, -1 = unknown (treat as committed) */
int mvcc_get_tx_state(DiskDB *db, uint64_t tx_id,
                      uint64_t *out_commit_ts);

/* Check visibility của một DocVersion đối với viewer transaction.
 * Đọc DocVersion header từ disk (page_id + offset) nếu cần.
 * Trả về VIS_VISIBLE / VIS_SKIP / VIS_NOT_FOUND / VIS_ERROR. */
VisibilityResult mvcc_check_visibility(DiskDB *db, MvccTx *viewer,
                                        const DocVersion *v);

/* Walk version chain từ version_ptr, tìm version đầu tiên visible với viewer.
 * Nếu tìm thấy: điền vào v_out và data_out (malloc'd, caller free), trả về 0.
 * Nếu không tìm thấy (hoặc deleted): trả về 1.
 * Lỗi IO: trả về -1. */
int mvcc_find_visible_version(DiskDB *db, MvccTx *viewer,
                               uint64_t version_ptr,
                               DocVersion *v_out,
                               uint8_t **data_out);

/* ===== High-level read API ===== */

/* Tìm document theo doc_id trong collection, visible với tx.
 * Trả về Document* (caller free) hoặc NULL nếu không thấy/deleted/error.
 * Đặt *err_out = 0 OK, -1 lỗi IO, 1 not found. */
Document* mvcc_find_doc(DiskDB *db, MvccTx *tx,
                        const char *coll_name, uint64_t doc_id,
                        int *err_out);

/* Scan callback cho mvcc_scan */
typedef void (*mvcc_visit_fn)(uint64_t id, Document *doc, void *ctx);

/* Scan toàn bộ collection, gọi fn cho mỗi document visible với tx.
 * Trả về 0 OK, -1 lỗi. */
int mvcc_scan(DiskDB *db, MvccTx *tx, const char *coll_name,
              mvcc_visit_fn fn, void *ctx);

/* ===== Page MVCC storage helpers ===== */

/* Đọc DocVersion header + data từ page tại offset.
 * data_out: caller cấp buffer hoặc NULL; nếu NULL function sẽ malloc.
 * Nếu malloc: caller free *data_out.
 * Trả về 0 OK, -1 lỗi. */
int mvcc_page_read_version(DiskDB *db, uint64_t page_id, uint16_t offset,
                           DocVersion *v_out, uint8_t **data_out);

/* Ghi DocVersion vào page mới (alloc page nếu cần).
 * Trả về version_ptr (page_id + offset) hoặc VERSION_PTR_NULL nếu lỗi. */
uint64_t mvcc_page_write_version(DiskDB *db, const DocVersion *v,
                                  const uint8_t *data);

/* Update field created_ts của version tại ptr (dùng khi commit). */
int mvcc_page_set_created_ts(DiskDB *db, uint64_t version_ptr, uint64_t created_ts);

/* Update field deleted_ts của version tại ptr (dùng khi delete/abort). */
int mvcc_page_set_deleted_ts(DiskDB *db, uint64_t version_ptr, uint64_t deleted_ts);

#endif
