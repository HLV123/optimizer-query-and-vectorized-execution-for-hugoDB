/* mvcc_recovery.c — MVCC WAL Recovery implementation */
#include "mvcc_recovery.h"
#include "mvcc_read.h"
#include "mvcc_write.h"
#include "mvcc_tx.h"
#include "doc_version.h"
#include "serializer.h"
#include "collection.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== Internal data structures ===== */

#define MVCC_REC_MAX_TX     4096
#define MVCC_REC_MAX_VER    65536

typedef struct {
    uint64_t tx_id;
    uint64_t begin_ts;
    uint64_t commit_ts;
    int      state;   /* 0=active/loser, 1=committed, 2=aborted */
} TxStateEntry;

typedef struct {
    uint64_t tx_id;
    uint64_t doc_id;
    uint64_t version_ptr;
    char     coll_name[64];
} VersionLogEntry;

/* ===== Helpers ===== */

static TxStateEntry* find_tx_entry(TxStateEntry *arr, int n, uint64_t tx_id) {
    for (int i = 0; i < n; i++) {
        if (arr[i].tx_id == tx_id) return &arr[i];
    }
    return NULL;
}

/* ===== mvcc_recover ===== */

int mvcc_recover(DiskDB *db) {
    if (!db->wal_enabled) return 0;
    if (db->wal.size == 0) return 0;

    printf("  [mvcc_recover] scanning WAL for MVCC records...\n");

    /* Alloc work buffers */
    TxStateEntry  *tx_map   = calloc(MVCC_REC_MAX_TX,  sizeof(TxStateEntry));
    VersionLogEntry *ver_log = calloc(MVCC_REC_MAX_VER, sizeof(VersionLogEntry));
    if (!tx_map || !ver_log) {
        free(tx_map); free(ver_log);
        return -1;
    }
    int n_tx  = 0;
    int n_ver = 0;

    uint64_t max_ts = 0;

    /* === Pass 1: scan WAL, build tx_map + ver_log === */
    WalIter it;
    wal_iter_init(&it, &db->wal);
    WalRecord rec;
    int rc;

    while ((rc = wal_iter_next(&it, &rec)) == WAL_OK) {
        switch (rec.type) {

        case WAL_MVCC_BEGIN: {
            /* page_id field = begin_ts */
            uint64_t begin_ts = rec.page_id;
            if (n_tx < MVCC_REC_MAX_TX) {
                TxStateEntry *e = find_tx_entry(tx_map, n_tx, rec.tx_id);
                if (!e) {
                    e = &tx_map[n_tx++];
                    e->tx_id     = rec.tx_id;
                    e->state     = 0;  /* active/loser until COMMIT/ABORT seen */
                }
                e->begin_ts = begin_ts;
                if (begin_ts > max_ts) max_ts = begin_ts;
            }
            break;
        }

        case WAL_MVCC_COMMIT: {
            /* page_id field = commit_ts */
            uint64_t commit_ts = rec.page_id;
            TxStateEntry *e = find_tx_entry(tx_map, n_tx, rec.tx_id);
            if (e) {
                e->commit_ts = commit_ts;
                e->state     = 1;  /* COMMITTED */
            } else if (n_tx < MVCC_REC_MAX_TX) {
                TxStateEntry *ne = &tx_map[n_tx++];
                ne->tx_id     = rec.tx_id;
                ne->commit_ts = commit_ts;
                ne->state     = 1;
            }
            if (commit_ts > max_ts) max_ts = commit_ts;
            break;
        }

        case WAL_MVCC_VERSION: {
            /* before[] = doc_id(8) + version_ptr(8) + coll_name */
            if (rec.length < 16) break;
            uint64_t doc_id      = read_u64_be(rec.before);
            uint64_t version_ptr = read_u64_be(rec.before + 8);
            char coll_name[64];
            memset(coll_name, 0, sizeof(coll_name));
            size_t clen = rec.length - 16;
            if (clen >= sizeof(coll_name)) clen = sizeof(coll_name) - 1;
            memcpy(coll_name, rec.before + 16, clen);

            if (n_ver < MVCC_REC_MAX_VER) {
                VersionLogEntry *ve = &ver_log[n_ver++];
                ve->tx_id       = rec.tx_id;
                ve->doc_id      = doc_id;
                ve->version_ptr = version_ptr;
                strncpy(ve->coll_name, coll_name, sizeof(ve->coll_name) - 1);
            }
            break;
        }

        case WAL_BEGIN:
        case WAL_ABORT: {
            /* Legacy 2PL abort hoặc MVCC abort via WAL_ABORT */
            TxStateEntry *e = find_tx_entry(tx_map, n_tx, rec.tx_id);
            if (rec.type == WAL_ABORT) {
                if (e) e->state = 2;
                else if (n_tx < MVCC_REC_MAX_TX) {
                    TxStateEntry *ne = &tx_map[n_tx++];
                    ne->tx_id = rec.tx_id;
                    ne->state = 2;
                }
            } else {
                /* WAL_BEGIN (legacy) */
                if (!e && n_tx < MVCC_REC_MAX_TX) {
                    TxStateEntry *ne = &tx_map[n_tx++];
                    ne->tx_id = rec.tx_id;
                    ne->state = 0;
                }
            }
            break;
        }

        default:
            break;
        }
    }

    /* === Pass 2: populate committed_table với tx state === */
    int n_committed = 0, n_aborted = 0, n_loser = 0;
    for (int i = 0; i < n_tx; i++) {
        TxStateEntry *e = &tx_map[i];
        if (e->state == 1) {
            mvcc_committed_table_add(&db->mvcc_committed, e->tx_id, e->commit_ts, 0);
            n_committed++;
        } else if (e->state == 2) {
            mvcc_committed_table_add(&db->mvcc_committed, e->tx_id, 0, 1);
            n_aborted++;
        } else {
            /* Loser: BEGIN tanpa COMMIT/ABORT → treat as ABORTED */
            mvcc_committed_table_add(&db->mvcc_committed, e->tx_id, 0, 1);
            n_loser++;
        }
    }

    /* === Pass 3: rebuild doc_page_ids từ version log ===
     *
     * Với mỗi (coll, doc_id), tìm version_ptr CUỐI CÙNG được ghi bởi
     * COMMITTED tx. Đây là latest visible version sau recovery.
     *
     * Nếu version được ghi bởi aborted/loser tx:
     *   - Đọc DocVersion.prev_version_ptr từ page
     *   - Restore doc_page_ids về prev_version_ptr
     */
    int n_restored = 0;
    for (int i = 0; i < n_ver; i++) {
        VersionLogEntry *ve = &ver_log[i];

        /* Tìm state của tx tạo version này */
        TxStateEntry *te = find_tx_entry(tx_map, n_tx, ve->tx_id);
        int is_aborted = (te == NULL) || (te->state != 1);

        DiskColl *c = ddb_get_coll(db, ve->coll_name);
        if (!c) continue;

        /* Ensure capacity */
        if (ve->doc_id >= c->capacity) {
            /* Inline grow */
            uint64_t new_cap = c->capacity ? c->capacity : 64;
            while (new_cap <= ve->doc_id) new_cap *= 2;
            uint64_t *nd = (uint64_t*)realloc(c->doc_page_ids,
                                               new_cap * sizeof(uint64_t));
            if (!nd) continue;
            memset(nd + c->capacity, 0, (new_cap - c->capacity) * sizeof(uint64_t));
            c->doc_page_ids = nd;
            c->capacity     = new_cap;
        }

        if (is_aborted) {
            /* Undo: đọc prev_version_ptr từ page và restore */
            DocVersion v;
            if (mvcc_page_read_version(db,
                                       version_ptr_page(ve->version_ptr),
                                       version_ptr_offset(ve->version_ptr),
                                       &v, NULL) == 0) {
                c->doc_page_ids[ve->doc_id] = v.prev_version_ptr;
                if (v.prev_version_ptr == VERSION_PTR_NULL) {
                    /* INSERT aborted: doc should not exist */
                    if (c->count > 0) c->count--;
                }
                n_restored++;
            }
        } else {
            /* Committed version: set doc_page_ids */
            c->doc_page_ids[ve->doc_id] = ve->version_ptr;

            /* Set created_ts từ commit_ts nếu chưa set */
            if (te && te->commit_ts > 0) {
                /* Verify current created_ts trên page */
                DocVersion v;
                if (mvcc_page_read_version(db,
                                           version_ptr_page(ve->version_ptr),
                                           version_ptr_offset(ve->version_ptr),
                                           &v, NULL) == 0) {
                    if (v.created_ts == 0) {
                        /* Crash trước khi set created_ts → fix now */
                        mvcc_page_set_created_ts(db, ve->version_ptr, te->commit_ts);
                    }
                }
            }
        }
    }

    /* === Pass 4: advance TsOracle vượt qua max_ts === */
    ts_oracle_advance(&db->mvcc_oracle, max_ts + 1);

    printf("  [mvcc_recover] tx: committed=%d, aborted=%d, loser=%d; "
           "versions: %d (%d restored); max_ts=%llu\n",
           n_committed, n_aborted, n_loser, n_ver, n_restored,
           (unsigned long long)max_ts);

    free(tx_map);
    free(ver_log);
    db->dirty = 1;
    return 0;
}
