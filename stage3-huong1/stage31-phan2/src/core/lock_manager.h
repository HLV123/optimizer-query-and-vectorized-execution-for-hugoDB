/* lock_manager.h — 2PL Lock Manager (single-thread logic version)
 *
 * Resource = uint64 (sẽ dùng làm page_id hoặc collection_id).
 * Tx_id = uint64 (cấp bởi WAL).
 *
 * Lock modes:
 *   LM_S — Shared (read): nhiều tx có thể giữ S đồng thời trên cùng resource
 *   LM_X — Exclusive (write): 1 tx duy nhất, không ai khác có lock cùng resource
 *
 * Conflict matrix:
 *           S       X
 *   S       OK      CONFLICT
 *   X       CONFLICT CONFLICT
 *
 * 2PL phase rule: trong 1 tx, sau khi đã RELEASE 1 lock, KHÔNG được ACQUIRE
 * thêm lock mới. Lock manager track per-tx state để enforce.
 *
 * Lock upgrade: nếu tx đang giữ S và xin X trên cùng resource → upgrade.
 *   - Nếu không tx nào khác giữ S → upgrade thành công ngay
 *   - Nếu có → wait
 *
 * Deadlock detection: wait-for graph.
 *   - Khi tx A wait tx B → cạnh A → B
 *   - Mỗi acquire wait → check cycle (DFS)
 *   - Nếu có cycle → return LM_DEADLOCK; caller phải abort tx
 *
 * Single-thread version: lm_acquire trả về:
 *   LM_OK         — granted ngay
 *   LM_WAIT       — phải chờ (caller xử lý: trong single-thread test
 *                    có nghĩa là conflict, caller có thể retry sau khi
 *                    tx khác release; trong multi-thread sẽ block)
 *   LM_DEADLOCK   — deadlock detected, caller phải abort
 *   LM_PHASE_VIOLATION — cố acquire sau khi đã release (vi phạm 2PL)
 */
#ifndef HUGO_LOCK_MANAGER_H
#define HUGO_LOCK_MANAGER_H

#include <stdint.h>

#define LM_S  0
#define LM_X  1

#define LM_OK                 0
#define LM_WAIT              -1
#define LM_DEADLOCK          -2
#define LM_PHASE_VIOLATION   -3
#define LM_NOT_HELD          -4
#define LM_NOMEM             -5

typedef uint64_t lm_tx_t;
typedef uint64_t lm_res_t;

typedef struct LockHolder {
    lm_tx_t  tx;
    int      mode;
    struct LockHolder *next;
} LockHolder;

typedef struct LockWaiter {
    lm_tx_t  tx;
    int      mode;
    struct LockWaiter *next;
} LockWaiter;

typedef struct LockEntry {
    lm_res_t   resource;
    LockHolder *holders;
    LockWaiter *waiters;
    struct LockEntry *next;   /* Hash chain */
} LockEntry;

typedef struct TxState {
    lm_tx_t  tx;
    int      shrinking;       /* 1 nếu đã release ≥ 1 lock — không acquire thêm */
    int      n_locks;         /* số lock đang giữ */
    struct TxState *next;
} TxState;

#define LM_HASH_SIZE  256

typedef struct {
    LockEntry  *table[LM_HASH_SIZE];   /* hash bucket */
    TxState    *tx_states;             /* linked list */
} LockManager;

void lm_init   (LockManager *lm);
void lm_destroy(LockManager *lm);

/* Acquire S/X lock. Trả về LM_OK / LM_WAIT / LM_DEADLOCK / LM_PHASE_VIOLATION. */
int  lm_acquire(LockManager *lm, lm_tx_t tx, lm_res_t res, int mode);

/* Release 1 lock cụ thể của tx trên resource */
int  lm_release(LockManager *lm, lm_tx_t tx, lm_res_t res);

/* Release tất cả lock của tx (gọi khi commit/abort) — đồng thời grant cho waiters */
int  lm_release_all(LockManager *lm, lm_tx_t tx);

/* Query: tx có giữ lock mode trên resource không? */
int  lm_holds(LockManager *lm, lm_tx_t tx, lm_res_t res, int mode);

/* Debug */
void lm_print(const LockManager *lm);

#endif
