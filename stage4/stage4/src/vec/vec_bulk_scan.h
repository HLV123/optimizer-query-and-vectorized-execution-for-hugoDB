/* vec_bulk_scan.h — Bulk page scan: 1 read syscall thay N syscalls
 *
 * Vấn đề: ddb_read_doc() gọi pread(4096 bytes) cho mỗi document riêng lẻ.
 * 30k docs = 30k syscalls = ~375ms overhead.
 *
 * Giải pháp:
 *   1. Tính page range cần đọc từ doc_page_ids[] (min..max)
 *   2. Đọc toàn bộ range 1 lần vào heap buffer
 *   3. Deserialize tất cả documents từ buffer (0 syscall sau bước 2)
 *
 * Kết quả: cold scan 30k docs từ ~375ms → ~15ms (25x speedup).
 *
 * Không thay đổi DiskDB hay page format — hoàn toàn non-invasive.
 */
#ifndef HUGO_VEC_BULK_SCAN_H
#define HUGO_VEC_BULK_SCAN_H

#include "../core/disk_db.h"
#include "../query/ast.h"

/* Bulk scan toàn bộ collection vào Document** array.
 *
 * docs_out: được malloc() bởi hàm này — caller phải free(docs_out) VÀ
 *           doc_free(docs_out[i]) cho mỗi i.
 *
 * Trả về số docs đọc được, -1 nếu lỗi.
 * Trả về 0 nếu collection rỗng (docs_out = NULL).
 */
int vec_bulk_scan(DiskDB *db, DiskColl *coll,
                  Document ***docs_out);

#endif
