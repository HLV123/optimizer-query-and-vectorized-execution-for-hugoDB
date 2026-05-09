/* wal.c — Write-Ahead Log
 *
 * Append-only file. Mỗi append:
 *   build buffer → write to file → update size + next_lsn
 *
 * Nếu crash giữa write → record bị cắt → recovery sẽ phát hiện qua:
 *   - read short trước khi đủ HDR → STOP (record cuối truncated)
 *   - read OK nhưng checksum sai → STOP
 *
 * Stop ở record đầu tiên fail = boundary giữa "đã commit log" và "chưa".
 * Mọi thứ trước đó coi là valid.
 */
#include "wal.h"
#include "serializer.h"
#include "checksum.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Offsets trong record */
#define WR_LSN_OFF      0
#define WR_TXID_OFF     8
#define WR_TYPE_OFF     16
#define WR_PAGEID_OFF   17
#define WR_OFFSET_OFF   25
#define WR_LENGTH_OFF   27
#define WR_IMAGES_OFF   29

static size_t record_size(uint16_t length) {
    return WAL_HDR_SIZE + 2 * (size_t)length + WAL_TAIL_SIZE;
}

/* ===== Lifecycle ===== */
int wal_open(Wal *w, const char *path) {
    w->file = hugo_open(path, HUGO_OPEN_RDWR | HUGO_OPEN_CREATE);
    if (!w->file) return WAL_ERR_IO;

    int64_t sz = hugo_size(w->file);
    if (sz < 0) { hugo_close(w->file); w->file = NULL; return WAL_ERR_IO; }
    w->size = (uint64_t)sz;

    /* Scan để tìm next_lsn và next_tx_id từ records có sẵn */
    w->next_lsn   = 1;
    w->next_tx_id = 1;

    if (sz > 0) {
        WalIter it;
        wal_iter_init(&it, w);
        WalRecord rec;
        uint64_t last_valid_offset = 0;
        uint64_t prev_offset = 0;
        int rc;
        while ((rc = wal_iter_next(&it, &rec)) == WAL_OK) {
            if (rec.lsn >= w->next_lsn) w->next_lsn = rec.lsn + 1;
            if (rec.tx_id >= w->next_tx_id) w->next_tx_id = rec.tx_id + 1;
            last_valid_offset = it.offset;
            prev_offset = it.offset;
        }
        (void)prev_offset;
        if (rc == WAL_ERR_TRUNC || rc == WAL_ERR_CHECKSUM) {
            /* Record cuối bị cắt/hỏng → truncate file về last_valid_offset
             * để các append sau không tiếp giữa byte rác.
             * Nhưng KHÔNG truncate ngay (cần preserve cho test).
             * Set size = last_valid_offset → mọi append về sau bắt đầu từ đó,
             * các byte rác sẽ bị overwrite.
             */
            w->size = last_valid_offset;
        }
    }
    return WAL_OK;
}

int wal_close(Wal *w) {
    if (!w || !w->file) return WAL_OK;
    hugo_close(w->file);
    w->file = NULL;
    return WAL_OK;
}

int wal_sync(Wal *w) {
    if (hugo_sync(w->file) != HUGO_OK) return WAL_ERR_IO;
    return WAL_OK;
}

int wal_truncate(Wal *w) {
    /* Đơn giản: ghi đè size = 0, các append sau bắt đầu từ 0.
     * (Không thực sự shrink file — OS sẽ thấy file vẫn cũ size,
     *  nhưng đọc sẽ stop tại record đầu tiên invalid.)
     * Production sẽ dùng SetEndOfFile / ftruncate. */
    w->size = 0;
    w->next_lsn = 1;
    return WAL_OK;
}

uint64_t wal_new_tx_id(Wal *w) {
    return w->next_tx_id++;
}

/* ===== Append ===== */
static int64_t wal_append(Wal *w, uint8_t type, uint64_t tx_id,
                           uint64_t page_id, uint16_t offset,
                           const uint8_t *before, const uint8_t *after,
                           uint16_t length) {
    if (length > WAL_MAX_DATA) return WAL_ERR_RANGE;

    size_t rsz = record_size(length);
    uint8_t *buf = (uint8_t*)malloc(rsz);
    if (!buf) return WAL_ERR_IO;
    memset(buf, 0, rsz);

    uint64_t lsn = w->next_lsn;

    write_u64_be(buf + WR_LSN_OFF,    lsn);
    write_u64_be(buf + WR_TXID_OFF,   tx_id);
    buf[WR_TYPE_OFF] = type;
    write_u64_be(buf + WR_PAGEID_OFF, page_id);
    write_u16_be(buf + WR_OFFSET_OFF, offset);
    write_u16_be(buf + WR_LENGTH_OFF, length);
    if (length > 0) {
        if (before) memcpy(buf + WR_IMAGES_OFF,           before, length);
        if (after)  memcpy(buf + WR_IMAGES_OFF + length,  after,  length);
    }
    /* Checksum: trên toàn record với 4 byte checksum field = 0 */
    size_t crc_off = rsz - WAL_TAIL_SIZE;
    /* các byte trc đó đã set, 4 byte cuối = 0 */
    uint32_t crc = hugo_crc32(buf, rsz);  /* checksum trên cả 4 byte 0 */
    write_u32_be(buf + crc_off, crc);

    int rc = hugo_write(w->file, buf, rsz, w->size);
    free(buf);
    if (rc != HUGO_OK) return WAL_ERR_IO;

    w->size += rsz;
    w->next_lsn++;
    return (int64_t)lsn;
}

int64_t wal_log_begin(Wal *w, uint64_t tx_id) {
    return wal_append(w, WAL_BEGIN, tx_id, 0, 0, NULL, NULL, 0);
}
int64_t wal_log_commit(Wal *w, uint64_t tx_id) {
    return wal_append(w, WAL_COMMIT, tx_id, 0, 0, NULL, NULL, 0);
}
int64_t wal_log_abort(Wal *w, uint64_t tx_id) {
    return wal_append(w, WAL_ABORT, tx_id, 0, 0, NULL, NULL, 0);
}
int64_t wal_log_update(Wal *w, uint64_t tx_id, uint64_t page_id,
                        uint16_t offset,
                        const uint8_t *before, const uint8_t *after,
                        uint16_t length) {
    return wal_append(w, WAL_UPDATE, tx_id, page_id, offset, before, after, length);
}
int64_t wal_log_checkpoint(Wal *w) {
    return wal_append(w, WAL_CHECKPOINT, 0, 0, 0, NULL, NULL, 0);
}

/* ===== Iterator ===== */
int wal_iter_init(WalIter *it, Wal *w) {
    it->w = w;
    it->offset = 0;
    it->end = w->size;
    return WAL_OK;
}

int wal_iter_next(WalIter *it, WalRecord *out) {
    if (it->offset >= it->end) return WAL_ERR_TRUNC;  /* hết log */

    /* Đọc header trước để biết length */
    uint8_t hdr[WAL_HDR_SIZE];
    if (it->offset + WAL_HDR_SIZE > it->end) return WAL_ERR_TRUNC;
    if (hugo_read(it->w->file, hdr, WAL_HDR_SIZE, it->offset) != HUGO_OK)
        return WAL_ERR_IO;

    uint16_t length = read_u16_be(hdr + WR_LENGTH_OFF);
    if (length > WAL_MAX_DATA) return WAL_ERR_CHECKSUM;  /* corrupt */

    size_t rsz = record_size(length);
    if (it->offset + rsz > it->end) return WAL_ERR_TRUNC;

    uint8_t *buf = (uint8_t*)malloc(rsz);
    if (!buf) return WAL_ERR_IO;
    if (hugo_read(it->w->file, buf, rsz, it->offset) != HUGO_OK) {
        free(buf); return WAL_ERR_IO;
    }

    /* Verify checksum */
    uint32_t stored = read_u32_be(buf + rsz - WAL_TAIL_SIZE);
    write_u32_be(buf + rsz - WAL_TAIL_SIZE, 0);
    uint32_t computed = hugo_crc32(buf, rsz);
    if (stored != computed) { free(buf); return WAL_ERR_CHECKSUM; }

    /* Parse */
    out->lsn      = read_u64_be(buf + WR_LSN_OFF);
    out->tx_id    = read_u64_be(buf + WR_TXID_OFF);
    out->type     = buf[WR_TYPE_OFF];
    out->page_id  = read_u64_be(buf + WR_PAGEID_OFF);
    out->offset   = read_u16_be(buf + WR_OFFSET_OFF);
    out->length   = length;
    if (length > 0) {
        memcpy(out->before, buf + WR_IMAGES_OFF,          length);
        memcpy(out->after,  buf + WR_IMAGES_OFF + length, length);
    }

    free(buf);
    it->offset += rsz;
    return WAL_OK;
}

/* ===== Recovery (ARIES, simplified) ===== */
static int is_committed_helper(uint64_t tx, uint64_t *arr, int n) {
    for (int i = 0; i < n; i++) if (arr[i] == tx) return 1;
    return 0;
}

int wal_recover(Wal *w, PageManager *pm) {
    /* Pass 1 — Analysis: scan log, build sets:
     *   committed_tx[], aborted_tx[]
     * Loser = tx có BEGIN nhưng KHÔNG có COMMIT và KHÔNG có ABORT.
     * tx có ABORT → ứng dụng đã rollback (UPDATE sau ABORT không có) →
     *   recovery không UNDO; nhưng REDO vẫn áp dụng cho UPDATE trước ABORT
     *   để đưa page về trạng thái nhất quán với log, rồi UNDO ngược các
     *   UPDATE này. Để đơn giản & đúng: với aborted tx, KHÔNG REDO và
     *   cũng KHÔNG UNDO (vì ứng dụng đã handle).
     *
     * Wait — vẫn phải UNDO các UPDATE của aborted tx, vì nếu page đã ghi
     * xuống disk trước crash, recovery cần khôi phục. Nhưng ứng dụng đã
     * ghi before_image trước khi ABORT — vậy disk đã đúng. Không cần UNDO.
     *
     * Để hoàn toàn đúng: aborted tx → không REDO (skip), không UNDO.
     */
    /* Dynamic: start at 4096, grow when full. Fix bug: với bulk import
     * 10k+ auto-tx, MAX_TX hardcoded 4096 khiến tx > 4096 bị coi là loser
     * và UNDO sai. */
    int MAX_TX = 4096;
    uint64_t *seen_begin     = calloc(MAX_TX, sizeof(uint64_t));
    uint64_t *seen_committed = calloc(MAX_TX, sizeof(uint64_t));
    uint64_t *seen_aborted   = calloc(MAX_TX, sizeof(uint64_t));
    int n_begin = 0, n_committed = 0, n_aborted = 0;
    if (!seen_begin || !seen_committed || !seen_aborted) {
        free(seen_begin); free(seen_committed); free(seen_aborted);
        return WAL_ERR_IO;
    }

    #define GROW_IF_FULL(arr, n) do {                                         \
        if ((n) >= MAX_TX) {                                                   \
            int new_cap = MAX_TX * 2;                                          \
            seen_begin     = realloc(seen_begin,     new_cap * sizeof(uint64_t)); \
            seen_committed = realloc(seen_committed, new_cap * sizeof(uint64_t)); \
            seen_aborted   = realloc(seen_aborted,   new_cap * sizeof(uint64_t)); \
            MAX_TX = new_cap;                                                  \
        }                                                                      \
    } while(0)

    WalIter it;
    wal_iter_init(&it, w);
    WalRecord rec;
    int rc;
    while ((rc = wal_iter_next(&it, &rec)) == WAL_OK) {
        if (rec.type == WAL_BEGIN) {
            GROW_IF_FULL(seen_begin, n_begin);
            seen_begin[n_begin++] = rec.tx_id;
        } else if (rec.type == WAL_COMMIT) {
            GROW_IF_FULL(seen_committed, n_committed);
            seen_committed[n_committed++] = rec.tx_id;
        } else if (rec.type == WAL_ABORT) {
            GROW_IF_FULL(seen_aborted, n_aborted);
            seen_aborted[n_aborted++] = rec.tx_id;
        }
    }

    /* Pass 2 — REDO: apply UPDATE.after của committed tx.
     * (Bỏ qua aborted vì ứng dụng đã rollback;
     *  bỏ qua loser thuần để Pass 3 UNDO xử.) */
    wal_iter_init(&it, w);
    int redo_count = 0;
    while ((rc = wal_iter_next(&it, &rec)) == WAL_OK) {
        if (rec.type != WAL_UPDATE) continue;
        if (!is_committed_helper(rec.tx_id, seen_committed, n_committed)) continue;
        if (rec.page_id == 0) continue;

        /* Nếu WAL tham chiếu page nằm ngoài header.page_count, extend.
         * Đây là trường hợp crash trước khi pm_flush_header chạy — page
         * đã ghi xuống disk nhưng header chưa update. */
        if (rec.page_id >= pm->hdr.page_count) {
            pm->hdr.page_count = rec.page_id + 1;
        }

        HugoPage page;
        if (pm_read_page(pm, rec.page_id, &page) != PG_OK) {
            memset(&page, 0, sizeof(page));
            page.page_id = (uint32_t)rec.page_id;
            page.page_type = PAGE_TYPE_LEAF;
        }
        if ((size_t)rec.offset + rec.length <= HUGO_PAGE_DATA_SIZE) {
            memcpy(page.data + rec.offset, rec.after, rec.length);
            pm_write_page(pm, &page);
            redo_count++;
        }
    }

    /* Pass 3 — UNDO: chỉ với loser THỰC SỰ (BEGIN, không COMMIT, không ABORT) */
    typedef struct { uint64_t page_id; uint16_t offset; uint16_t length;
                     uint8_t before[WAL_MAX_DATA]; uint64_t tx_id; } UndoEntry;
    UndoEntry *undos = malloc(sizeof(UndoEntry) * 100000);
    int n_undos = 0;
    if (!undos) { free(seen_begin); free(seen_committed); free(seen_aborted); return WAL_ERR_IO; }

    wal_iter_init(&it, w);
    while ((rc = wal_iter_next(&it, &rec)) == WAL_OK) {
        if (rec.type != WAL_UPDATE) continue;
        if (is_committed_helper(rec.tx_id, seen_committed, n_committed)) continue;
        if (is_committed_helper(rec.tx_id, seen_aborted, n_aborted)) continue;
        if (n_undos >= 100000) break;
        undos[n_undos].page_id = rec.page_id;
        undos[n_undos].offset  = rec.offset;
        undos[n_undos].length  = rec.length;
        undos[n_undos].tx_id   = rec.tx_id;
        memcpy(undos[n_undos].before, rec.before, rec.length);
        n_undos++;
    }
    int undo_count = 0;
    for (int i = n_undos - 1; i >= 0; i--) {
        UndoEntry *u = &undos[i];
        if (u->page_id == 0 || u->page_id >= pm->hdr.page_count) continue;
        HugoPage page;
        if (pm_read_page(pm, u->page_id, &page) != PG_OK) continue;
        if ((size_t)u->offset + u->length <= HUGO_PAGE_DATA_SIZE) {
            memcpy(page.data + u->offset, u->before, u->length);
            pm_write_page(pm, &page);
            undo_count++;
        }
    }

    pm_flush_header(pm);
    hugo_sync(pm->file);

    int n_loser = n_begin - n_committed - n_aborted;
    printf("  [recovery] REDO=%d, UNDO=%d, committed=%d, aborted=%d, loser=%d\n",
           redo_count, undo_count, n_committed, n_aborted, n_loser);

    free(undos);
    free(seen_begin);
    free(seen_committed);
    free(seen_aborted);
    return WAL_OK;
}
