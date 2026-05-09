/* mvcc_tx.h — MVCC Transaction state
 *
 * MvccTx là transaction trong MVCC mode, lưu:
 *   - begin_ts: snapshot timestamp khi tx bắt đầu
 *   - commit_ts: set khi commit (0 = chưa commit)
 *   - active_set: danh sách tx_id đang ACTIVE lúc tx này begin
 *     → dùng để quyết định visibility: version tạo bởi tx trong active_set
 *        là "in-flight" tại snapshot của ta, không visible
 *   - write_set: danh sách (doc_key, version_ptr) ta đã tạo
 *     → dùng để set created_ts khi commit, hoặc mark aborted khi abort
 *   - state: ACTIVE / COMMITTED / ABORTED
 *
 * Active transaction registry (MvccTxRegistry):
 *   Global registry trong DiskDB lưu tất cả MVCC transactions đang ACTIVE.
 *   Khi tx begin, snapshot active set từ registry.
 *   Khi tx commit/abort, xóa khỏi registry.
 *
 * Isolation level: Snapshot Isolation (không phải Serializable).
 *   - Đọc nhìn thấy snapshot tại begin_ts (không có dirty read)
 *   - Write-write conflict: first-committer-wins
 *   - Write skew có thể xảy ra (known limitation của SI)
 */
#ifndef HUGO_MVCC_TX_H
#define HUGO_MVCC_TX_H

#include <stdint.h>
#include <stddef.h>

/* ===== Trạng thái transaction ===== */
typedef enum {
    MVCC_TX_ACTIVE    = 0,
    MVCC_TX_COMMITTED = 1,
    MVCC_TX_ABORTED   = 2,
} MvccTxState;

/* ===== Write set entry ===== */
/* Mỗi version ta tạo trong tx này — cần set created_ts khi commit */
typedef struct MvccWriteEntry {
    char     coll_name[64];      /* tên collection */
    uint64_t doc_id;             /* id của document */
    uint64_t version_ptr;        /* page+offset pointer tới DocVersion đã tạo */
} MvccWriteEntry;

#define MVCC_WRITE_SET_INIT_CAP  8

/* ===== MVCC Transaction ===== */
typedef struct MvccTx {
    uint64_t      tx_id;        /* unique tx_id (từ wal_new_tx_id) */
    MvccTxState   state;

    /* Snapshot timestamps */
    uint64_t      begin_ts;     /* lấy từ ts_oracle tại thời điểm begin */
    uint64_t      commit_ts;    /* 0 cho đến khi commit */

    /* Active set: danh sách tx_id đang ACTIVE tại thời điểm ta begin.
     * Versions được tạo bởi các tx này không visible với ta (chưa commit). */
    uint64_t     *active_set;   /* malloc'd array */
    size_t        n_active;

    /* Write set: các versions ta đã tạo (để set commit_ts hoặc abort) */
    MvccWriteEntry *write_set;
    size_t          n_writes;
    size_t          cap_writes;
} MvccTx;

/* ===== MvccTx lifecycle ===== */
MvccTx* mvcc_tx_create(uint64_t tx_id, uint64_t begin_ts,
                       const uint64_t *active_set, size_t n_active);
void    mvcc_tx_free  (MvccTx *tx);

/* Thêm version vào write set */
int     mvcc_tx_track_write(MvccTx *tx, const char *coll_name,
                            uint64_t doc_id, uint64_t version_ptr);

/* ===== Active transaction registry ===== */
/* Lưu danh sách tất cả MVCC tx đang ACTIVE trong DB */

#define MVCC_REGISTRY_CAP  256   /* tối đa 256 concurrent MVCC tx */

typedef struct {
    uint64_t  tx_ids[MVCC_REGISTRY_CAP];  /* tx_id của các tx đang active */
    MvccTx   *txs[MVCC_REGISTRY_CAP];     /* pointer tới MvccTx struct */
    int       count;
} MvccTxRegistry;

void     mvcc_registry_init  (MvccTxRegistry *reg);

/* Đăng ký tx mới khi begin */
int      mvcc_registry_add   (MvccTxRegistry *reg, MvccTx *tx);

/* Lấy snapshot active tx_id list tại thời điểm này (để pass cho mvcc_tx_create) */
void     mvcc_registry_snapshot(const MvccTxRegistry *reg,
                                 uint64_t *out_ids, size_t *out_n, size_t max);

/* Xóa tx khỏi registry khi commit/abort */
void     mvcc_registry_remove(MvccTxRegistry *reg, uint64_t tx_id);

/* Lookup tx theo tx_id (NULL nếu không tìm thấy) */
MvccTx*  mvcc_registry_find  (const MvccTxRegistry *reg, uint64_t tx_id);

/* Kiểm tra tx_id có trong registry (đang active) không */
int      mvcc_registry_is_active(const MvccTxRegistry *reg, uint64_t tx_id);

/* ===== Committed/Aborted tx state table =====
 *
 * Để check visibility, cần biết commit_ts của tx đã committed.
 * Lưu bảng đơn giản (circular buffer) cho các tx đã finish.
 *
 * Giới hạn: chỉ nhớ N tx gần nhất. Khi GC vacuum chạy, các version
 * cũ hơn oldest_visible_ts sẽ được clean up nên không cần tra cứu xa hơn.
 */
#define MVCC_COMMITTED_TABLE_SIZE  1024

typedef struct {
    uint64_t tx_id;
    uint64_t commit_ts;   /* 0 nếu aborted */
    int      aborted;
} MvccTxRecord;

typedef struct {
    MvccTxRecord entries[MVCC_COMMITTED_TABLE_SIZE];
    int          head;    /* circular buffer write head */
    int          count;
} MvccCommittedTable;

void     mvcc_committed_table_init  (MvccCommittedTable *tbl);
void     mvcc_committed_table_add   (MvccCommittedTable *tbl,
                                     uint64_t tx_id, uint64_t commit_ts, int aborted);
/* Tìm record cho tx_id. Trả về NULL nếu không có trong bảng (TX quá cũ → treat as committed) */
const MvccTxRecord* mvcc_committed_table_find(const MvccCommittedTable *tbl, uint64_t tx_id);

#endif
