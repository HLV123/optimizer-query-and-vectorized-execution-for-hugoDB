/* lock_manager.c — Lock manager implementation */
#include "lock_manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ===== Hash ===== */
static unsigned hash_res(lm_res_t r) {
    /* simple */
    r ^= r >> 33;
    r *= 0xff51afd7ed558ccdULL;
    r ^= r >> 33;
    return (unsigned)(r & (LM_HASH_SIZE - 1));
}

/* ===== Init / destroy ===== */
void lm_init(LockManager *lm) {
    for (int i = 0; i < LM_HASH_SIZE; i++) lm->table[i] = NULL;
    lm->tx_states = NULL;
}

static void free_holders(LockHolder *h) {
    while (h) { LockHolder *n = h->next; free(h); h = n; }
}
static void free_waiters(LockWaiter *w) {
    while (w) { LockWaiter *n = w->next; free(w); w = n; }
}

void lm_destroy(LockManager *lm) {
    for (int i = 0; i < LM_HASH_SIZE; i++) {
        LockEntry *e = lm->table[i];
        while (e) {
            LockEntry *n = e->next;
            free_holders(e->holders);
            free_waiters(e->waiters);
            free(e);
            e = n;
        }
        lm->table[i] = NULL;
    }
    TxState *s = lm->tx_states;
    while (s) { TxState *n = s->next; free(s); s = n; }
    lm->tx_states = NULL;
}

/* ===== TxState helpers ===== */
static TxState* get_tx(LockManager *lm, lm_tx_t tx) {
    for (TxState *s = lm->tx_states; s; s = s->next)
        if (s->tx == tx) return s;
    TxState *s = (TxState*)calloc(1, sizeof(TxState));
    if (!s) return NULL;
    s->tx = tx;
    s->next = lm->tx_states;
    lm->tx_states = s;
    return s;
}

static void remove_tx(LockManager *lm, lm_tx_t tx) {
    TxState **pp = &lm->tx_states;
    while (*pp) {
        if ((*pp)->tx == tx) {
            TxState *del = *pp;
            *pp = del->next;
            free(del);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ===== Lock entry helpers ===== */
static LockEntry* find_entry(LockManager *lm, lm_res_t res) {
    unsigned h = hash_res(res);
    for (LockEntry *e = lm->table[h]; e; e = e->next)
        if (e->resource == res) return e;
    return NULL;
}

static LockEntry* get_or_create_entry(LockManager *lm, lm_res_t res) {
    LockEntry *e = find_entry(lm, res);
    if (e) return e;
    unsigned h = hash_res(res);
    e = (LockEntry*)calloc(1, sizeof(LockEntry));
    if (!e) return NULL;
    e->resource = res;
    e->next = lm->table[h];
    lm->table[h] = e;
    return e;
}

static void remove_entry_if_empty(LockManager *lm, LockEntry *e) {
    if (e->holders || e->waiters) return;
    unsigned h = hash_res(e->resource);
    LockEntry **pp = &lm->table[h];
    while (*pp) {
        if (*pp == e) { *pp = e->next; free(e); return; }
        pp = &(*pp)->next;
    }
}

/* Compatibility check: holders[] vs requested mode.
 * Trả về 1 nếu compatible (granted), 0 nếu conflict.
 * Self-holder của cùng tx luôn compatible. */
static int compatible(const LockEntry *e, lm_tx_t tx, int mode) {
    for (LockHolder *h = e->holders; h; h = h->next) {
        if (h->tx == tx) continue;  /* skip self */
        /* Conflict matrix */
        if (mode == LM_X) return 0;       /* X conflict với mọi mode khác */
        if (h->mode == LM_X) return 0;    /* khác tx giữ X → conflict */
    }
    return 1;
}

static LockHolder* find_holder(LockEntry *e, lm_tx_t tx) {
    for (LockHolder *h = e->holders; h; h = h->next)
        if (h->tx == tx) return h;
    return NULL;
}

/* ===== Wait-for graph & deadlock detection =====
 * Edge tx_A → tx_B nếu A đang wait B (B đang giữ lock A muốn).
 * Mỗi lần tx muốn wait → check xem thêm cạnh có tạo cycle không.
 *
 * Build implicit từ trạng thái lock table, không lưu sẵn.
 * Detect bằng DFS từ "start" tìm đường về "start" qua các cạnh ngầm.
 */

/* Helper: nếu tx X muốn mode trên res, list các tx incompatible đang giữ */
static int collect_blockers(LockEntry *e, lm_tx_t tx, int mode,
                             lm_tx_t *out, int max_out) {
    int n = 0;
    for (LockHolder *h = e->holders; h; h = h->next) {
        if (h->tx == tx) continue;
        int conflict = 0;
        if (mode == LM_X) conflict = 1;
        else if (h->mode == LM_X) conflict = 1;
        if (conflict && n < max_out) out[n++] = h->tx;
    }
    /* Cũng phải wait sau các waiter đứng trước (tránh starvation),
     * nhưng MVP single-thread: bỏ qua, chỉ cần holders đủ. */
    return n;
}

/* Tìm tất cả tx mà 'tx' đang chờ (qua các waiter entries của nó) */
static int outgoing_edges(LockManager *lm, lm_tx_t tx,
                           lm_tx_t *out, int max_out) {
    int n = 0;
    for (int i = 0; i < LM_HASH_SIZE; i++) {
        for (LockEntry *e = lm->table[i]; e; e = e->next) {
            for (LockWaiter *w = e->waiters; w; w = w->next) {
                if (w->tx != tx) continue;
                /* tx đang wait trên resource này → đợi mọi blocker */
                lm_tx_t blockers[64];
                int nb = collect_blockers(e, tx, w->mode, blockers, 64);
                for (int j = 0; j < nb && n < max_out; j++) {
                    out[n++] = blockers[j];
                }
            }
        }
    }
    return n;
}

/* DFS — kiểm tra có path từ 'cur' về 'target' không */
static int has_path(LockManager *lm, lm_tx_t cur, lm_tx_t target,
                     lm_tx_t *visited, int *n_visited, int max_visited) {
    if (cur == target) return 1;
    /* Đã visit? */
    for (int i = 0; i < *n_visited; i++)
        if (visited[i] == cur) return 0;
    if (*n_visited >= max_visited) return 0;
    visited[(*n_visited)++] = cur;

    lm_tx_t edges[64];
    int n_edges = outgoing_edges(lm, cur, edges, 64);
    for (int i = 0; i < n_edges; i++) {
        if (has_path(lm, edges[i], target, visited, n_visited, max_visited))
            return 1;
    }
    return 0;
}

/* Nếu giả định 'tx' wait 'blockers[]', có tạo cycle không?
 * Cycle = có blocker mà chính nó (qua các edge có sẵn) wait về tx. */
static int would_cause_cycle(LockManager *lm, lm_tx_t tx,
                              lm_tx_t *blockers, int n_blockers) {
    for (int i = 0; i < n_blockers; i++) {
        lm_tx_t visited[256];
        int nv = 0;
        if (has_path(lm, blockers[i], tx, visited, &nv, 256))
            return 1;
    }
    return 0;
}

/* ===== After release: try grant waiters ===== */
static void try_grant_waiters(LockEntry *e) {
    /* Scan từ đầu queue. Cấp lock cho waiters mà compatible với holders hiện tại
     * (và với các waiter đã được grant trong loop này).
     * Lưu ý: nếu đứng đầu queue là X waiter mà có S holder → blocked,
     * KHÔNG bỏ qua để grant waiter S sau (tránh starvation). */
    LockWaiter **pp = &e->waiters;
    while (*pp) {
        LockWaiter *w = *pp;
        if (!compatible(e, w->tx, w->mode)) {
            /* Đứng đầu queue đang block → dừng (FIFO fairness) */
            break;
        }
        /* Grant: thêm vào holders */
        LockHolder *h = find_holder(e, w->tx);
        if (h) {
            /* Đã có holder → upgrade S → X */
            if (w->mode == LM_X) h->mode = LM_X;
        } else {
            LockHolder *nh = (LockHolder*)calloc(1, sizeof(LockHolder));
            nh->tx = w->tx;
            nh->mode = w->mode;
            nh->next = e->holders;
            e->holders = nh;
        }
        /* Remove khỏi waiter queue */
        *pp = w->next;
        free(w);
    }
}

/* ===== Public API ===== */
int lm_acquire(LockManager *lm, lm_tx_t tx, lm_res_t res, int mode) {
    TxState *st = get_tx(lm, tx);
    if (!st) return LM_NOMEM;
    if (st->shrinking) return LM_PHASE_VIOLATION;

    LockEntry *e = get_or_create_entry(lm, res);
    if (!e) return LM_NOMEM;

    /* Check existing holder của tx này */
    LockHolder *self = find_holder(e, tx);
    if (self) {
        if (self->mode == LM_X || mode == LM_S) {
            /* Đã đủ mạnh */
            return LM_OK;
        }
        /* self->mode == S, mode == X → upgrade */
        /* Compatible nếu không tx khác giữ S */
        int blocked = 0;
        for (LockHolder *h = e->holders; h; h = h->next) {
            if (h->tx != tx) { blocked = 1; break; }
        }
        if (!blocked) {
            self->mode = LM_X;
            return LM_OK;
        }
        /* Upgrade phải wait → deadlock check */
        lm_tx_t blockers[64];
        int n = 0;
        for (LockHolder *h = e->holders; h; h = h->next)
            if (h->tx != tx && n < 64) blockers[n++] = h->tx;
        if (would_cause_cycle(lm, tx, blockers, n)) return LM_DEADLOCK;
        /* Add waiter */
        LockWaiter *w = (LockWaiter*)calloc(1, sizeof(LockWaiter));
        w->tx = tx; w->mode = LM_X;
        /* Append cuối queue */
        LockWaiter **pp = &e->waiters;
        while (*pp) pp = &(*pp)->next;
        *pp = w;
        return LM_WAIT;
    }

    /* Chưa có self-holder. Check compatible */
    if (compatible(e, tx, mode) && e->waiters == NULL) {
        /* Grant ngay (chỉ khi không có waiter đứng trước — fairness) */
        LockHolder *h = (LockHolder*)calloc(1, sizeof(LockHolder));
        h->tx = tx; h->mode = mode;
        h->next = e->holders;
        e->holders = h;
        st->n_locks++;
        return LM_OK;
    }

    /* Conflict → wait. Deadlock check. */
    lm_tx_t blockers[64];
    int n = collect_blockers(e, tx, mode, blockers, 64);
    if (would_cause_cycle(lm, tx, blockers, n)) return LM_DEADLOCK;

    LockWaiter *w = (LockWaiter*)calloc(1, sizeof(LockWaiter));
    w->tx = tx; w->mode = mode;
    LockWaiter **pp = &e->waiters;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    return LM_WAIT;
}

int lm_release(LockManager *lm, lm_tx_t tx, lm_res_t res) {
    LockEntry *e = find_entry(lm, res);
    if (!e) return LM_NOT_HELD;

    LockHolder **pp = &e->holders;
    int found = 0;
    while (*pp) {
        if ((*pp)->tx == tx) {
            LockHolder *del = *pp;
            *pp = del->next;
            free(del);
            found = 1;
            break;
        }
        pp = &(*pp)->next;
    }
    if (!found) return LM_NOT_HELD;

    TxState *st = get_tx(lm, tx);
    if (st) {
        st->n_locks--;
        st->shrinking = 1;   /* 2PL: đã release → chuyển shrinking */
    }
    try_grant_waiters(e);
    remove_entry_if_empty(lm, e);
    return LM_OK;
}

int lm_release_all(LockManager *lm, lm_tx_t tx) {
    /* Scan toàn bộ table */
    int n_released = 0;
    for (int i = 0; i < LM_HASH_SIZE; i++) {
        LockEntry **bpp = &lm->table[i];
        while (*bpp) {
            LockEntry *e = *bpp;
            /* Remove holders của tx */
            LockHolder **pp = &e->holders;
            while (*pp) {
                if ((*pp)->tx == tx) {
                    LockHolder *del = *pp;
                    *pp = del->next;
                    free(del);
                    n_released++;
                } else {
                    pp = &(*pp)->next;
                }
            }
            /* Remove waiters của tx */
            LockWaiter **wpp = &e->waiters;
            while (*wpp) {
                if ((*wpp)->tx == tx) {
                    LockWaiter *del = *wpp;
                    *wpp = del->next;
                    free(del);
                } else {
                    wpp = &(*wpp)->next;
                }
            }
            try_grant_waiters(e);
            if (!e->holders && !e->waiters) {
                *bpp = e->next;
                free(e);
            } else {
                bpp = &e->next;
            }
        }
    }
    remove_tx(lm, tx);
    return n_released;
}

int lm_holds(LockManager *lm, lm_tx_t tx, lm_res_t res, int mode) {
    LockEntry *e = find_entry(lm, res);
    if (!e) return 0;
    LockHolder *h = find_holder(e, tx);
    if (!h) return 0;
    if (mode == LM_S) return 1;       /* X cũng tính là giữ S */
    return h->mode == LM_X;
}

void lm_print(const LockManager *lm) {
    printf("Lock table:\n");
    int n = 0;
    for (int i = 0; i < LM_HASH_SIZE; i++) {
        for (LockEntry *e = lm->table[i]; e; e = e->next) {
            printf("  res=%llu holders=[", (unsigned long long)e->resource);
            for (LockHolder *h = e->holders; h; h = h->next) {
                printf("(%llu,%c)", (unsigned long long)h->tx,
                       h->mode == LM_X ? 'X' : 'S');
            }
            printf("] waiters=[");
            for (LockWaiter *w = e->waiters; w; w = w->next) {
                printf("(%llu,%c)", (unsigned long long)w->tx,
                       w->mode == LM_X ? 'X' : 'S');
            }
            printf("]\n");
            n++;
        }
    }
    if (n == 0) printf("  (empty)\n");
}
