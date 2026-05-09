/* wal.h — Write-Ahead Log
 *
 * Mỗi record format (variable length):
 *   [0..7]   lsn          u64 BE
 *   [8..15]  tx_id        u64 BE
 *   [16]     type         u8     (WAL_BEGIN/COMMIT/ABORT/UPDATE/CHECKPOINT)
 *   [17..24] page_id      u64 BE
 *   [25..26] offset       u16 BE
 *   [27..28] length       u16 BE  (bytes của before/after image)
 *   [29..]   before_image (length bytes)
 *   [...]    after_image  (length bytes)
 *   [...]    checksum     u32 BE  (CRC32 toàn record với checksum field = 0)
 *
 * Total record size = 33 + 2*length + 4
 *
 * Thứ tự bắt buộc khi modify 1 page:
 *   1. wal_log_update(wal, tx_id, page_id, offset, before, after, length)
 *   2. wal_sync(wal)                          ← fsync log
 *   3. (sau) ghi data page xuống disk         ← qua BP/PM
 *
 * Crash recovery sau restart:
 *   wal_recover(wal, pm) → quét log, REDO + UNDO
 */
#ifndef HUGO_WAL_H
#define HUGO_WAL_H

#include <stdint.h>
#include <stddef.h>
#include "page.h"

/* Record types */
#define WAL_BEGIN       1
#define WAL_COMMIT      2
#define WAL_ABORT       3
#define WAL_UPDATE      4
#define WAL_CHECKPOINT  5

#define WAL_MAX_DATA    HUGO_PAGE_DATA_SIZE   /* tối đa = 1 page */

/* Header size (không kể image data + checksum) */
#define WAL_HDR_SIZE    29
#define WAL_TAIL_SIZE   4   /* checksum */

/* Result codes */
#define WAL_OK            0
#define WAL_ERR_IO       -1
#define WAL_ERR_CHECKSUM -2
#define WAL_ERR_RANGE    -3
#define WAL_ERR_TRUNC    -4   /* record bị cắt giữa chừng (crash trong write) */

typedef struct {
    HugoFile *file;
    uint64_t  next_lsn;   /* lsn sẽ gán cho record kế tiếp */
    uint64_t  next_tx_id; /* tx_id sẽ gán cho BEGIN tiếp theo */
    uint64_t  size;       /* current size of log file (offset để append) */
} Wal;

/* Một record đọc lên RAM */
typedef struct {
    uint64_t lsn;
    uint64_t tx_id;
    uint8_t  type;
    uint64_t page_id;
    uint16_t offset;
    uint16_t length;
    uint8_t  before[WAL_MAX_DATA];
    uint8_t  after[WAL_MAX_DATA];
} WalRecord;

/* Lifecycle */
int  wal_open  (Wal *w, const char *path);   /* tạo nếu chưa có */
int  wal_close (Wal *w);
int  wal_sync  (Wal *w);                     /* fsync */
int  wal_truncate(Wal *w);                   /* xoá log sau checkpoint */

/* Append helpers — return: lsn của record vừa ghi (>0), hoặc <0 lỗi */
int64_t wal_log_begin   (Wal *w, uint64_t tx_id);
int64_t wal_log_commit  (Wal *w, uint64_t tx_id);
int64_t wal_log_abort   (Wal *w, uint64_t tx_id);
int64_t wal_log_update  (Wal *w, uint64_t tx_id, uint64_t page_id,
                         uint16_t offset,
                         const uint8_t *before, const uint8_t *after,
                         uint16_t length);
int64_t wal_log_checkpoint(Wal *w);

/* Cấp tx_id mới (đơn điệu) */
uint64_t wal_new_tx_id(Wal *w);

/* Recovery — dùng PageManager để apply REDO/UNDO lên data file */
int  wal_recover(Wal *w, PageManager *pm);

/* Iterator — đọc tuần tự records từ đầu file. Dùng cho test + recovery. */
typedef struct {
    Wal      *w;
    uint64_t  offset;
    uint64_t  end;
} WalIter;

int wal_iter_init(WalIter *it, Wal *w);
int wal_iter_next(WalIter *it, WalRecord *out);   /* WAL_OK / WAL_ERR_TRUNC nếu hết / khác = lỗi */

#endif
