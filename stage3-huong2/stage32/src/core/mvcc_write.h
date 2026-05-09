/* mvcc_write.h — MVCC Write Path
 *
 * INSERT/UPDATE/DELETE trong MVCC mode: tạo version mới thay vì sửa in-place.
 *
 * INSERT:
 *   - Tạo DocVersion với prev_ptr = VERSION_PTR_NULL
 *   - created_tx = tx->tx_id; created_ts = 0 (uncommitted)
 *   - Ghi vào page mới
 *   - doc_page_ids[new_id] = version_ptr
 *   - Track trong write_set
 *
 * UPDATE:
 *   - Check write-write conflict (section 3.6): first-committer-wins
 *   - Tạo DocVersion MỚI với prev_ptr = current latest version_ptr
 *   - doc_page_ids[doc_id] = new_version_ptr (latest pointer update)
 *   - Track trong write_set
 *
 * DELETE:
 *   - Tạo DocVersion với deleted_ts = UINT64_MAX (placeholder) và data_size = 0
 *   - Thực ra: set deleted_ts trên latest version
 *   - Khi commit: set deleted_ts = commit_ts
 *   - Khi abort: set deleted_ts = 0 (undo)
 *
 * COMMIT:
 *   - Lấy commit_ts = ts_oracle_next()
 *   - Với mỗi entry trong write_set: set created_ts = commit_ts (hoặc deleted_ts)
 *   - Xóa tx khỏi registry, thêm vào committed table
 *
 * ABORT:
 *   - Với mỗi entry trong write_set:
 *     - Nếu là INSERT: xóa doc_page_ids (hoặc mark version aborted)
 *     - Nếu là UPDATE: restore doc_page_ids về version trước
 *     - Nếu là DELETE: set deleted_ts = 0 (undo delete)
 *   - Xóa tx khỏi registry, thêm vào committed table (aborted=1)
 */
#ifndef HUGO_MVCC_WRITE_H
#define HUGO_MVCC_WRITE_H

#include "disk_db.h"
#include "mvcc_tx.h"
#include "../query/ast.h"

/* Return codes */
#define MVCC_OK              0
#define MVCC_ERR_IO         -1
#define MVCC_ERR_CONFLICT   -2   /* write-write conflict → abort tx */
#define MVCC_ERR_NOTFOUND   -3
#define MVCC_ERR_NOMEM      -4
#define MVCC_ERR_TOOLARGE   -5

/* INSERT document mới.
 * Trả về MVCC_OK và set *out_id, hoặc MVCC_ERR_*. */
int mvcc_insert_doc(DiskDB *db, MvccTx *tx, const char *coll_name,
                    Document *doc, uint64_t *out_id);

/* UPDATE document tại doc_id.
 * Trả về MVCC_OK hoặc MVCC_ERR_CONFLICT / MVCC_ERR_NOTFOUND / MVCC_ERR_IO. */
int mvcc_update_doc(DiskDB *db, MvccTx *tx, const char *coll_name,
                    uint64_t doc_id, Document *new_doc);

/* DELETE document tại doc_id.
 * Trả về MVCC_OK hoặc MVCC_ERR_CONFLICT / MVCC_ERR_NOTFOUND / MVCC_ERR_IO. */
int mvcc_delete_doc(DiskDB *db, MvccTx *tx, const char *coll_name,
                    uint64_t doc_id);

/* COMMIT transaction:
 *   - Gán commit_ts cho tất cả versions trong write_set
 *   - Cleanup registry
 * Trả về MVCC_OK hoặc MVCC_ERR_IO. */
int mvcc_commit_tx(DiskDB *db, MvccTx *tx);

/* ABORT transaction:
 *   - Undo tất cả writes: restore doc_page_ids, clear deleted_ts
 *   - Cleanup registry
 * Trả về MVCC_OK. */
int mvcc_abort_tx(DiskDB *db, MvccTx *tx);

#endif
