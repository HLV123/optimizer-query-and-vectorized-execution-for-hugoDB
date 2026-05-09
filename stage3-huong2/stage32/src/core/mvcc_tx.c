/* mvcc_tx.c — MVCC Transaction implementation */
#include "mvcc_tx.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== MvccTx lifecycle ===== */

MvccTx* mvcc_tx_create(uint64_t tx_id, uint64_t begin_ts,
                       const uint64_t *active_set, size_t n_active) {
    MvccTx *tx = (MvccTx*)calloc(1, sizeof(MvccTx));
    if (!tx) return NULL;

    tx->tx_id     = tx_id;
    tx->state     = MVCC_TX_ACTIVE;
    tx->begin_ts  = begin_ts;
    tx->commit_ts = 0;

    /* Copy active set */
    tx->n_active = n_active;
    if (n_active > 0 && active_set) {
        tx->active_set = (uint64_t*)malloc(n_active * sizeof(uint64_t));
        if (!tx->active_set) { free(tx); return NULL; }
        memcpy(tx->active_set, active_set, n_active * sizeof(uint64_t));
    } else {
        tx->active_set = NULL;
    }

    /* Init write set */
    tx->cap_writes = MVCC_WRITE_SET_INIT_CAP;
    tx->n_writes   = 0;
    tx->write_set  = (MvccWriteEntry*)malloc(tx->cap_writes * sizeof(MvccWriteEntry));
    if (!tx->write_set) {
        free(tx->active_set);
        free(tx);
        return NULL;
    }

    return tx;
}

void mvcc_tx_free(MvccTx *tx) {
    if (!tx) return;
    free(tx->active_set);
    free(tx->write_set);
    free(tx);
}

int mvcc_tx_track_write(MvccTx *tx, const char *coll_name,
                        uint64_t doc_id, uint64_t version_ptr) {
    /* Grow write set nếu cần */
    if (tx->n_writes >= tx->cap_writes) {
        size_t new_cap = tx->cap_writes * 2;
        MvccWriteEntry *new_ws = (MvccWriteEntry*)realloc(
            tx->write_set, new_cap * sizeof(MvccWriteEntry));
        if (!new_ws) return -1;
        tx->write_set  = new_ws;
        tx->cap_writes = new_cap;
    }

    MvccWriteEntry *e = &tx->write_set[tx->n_writes++];
    strncpy(e->coll_name, coll_name, sizeof(e->coll_name) - 1);
    e->coll_name[sizeof(e->coll_name)-1] = '\0';
    e->doc_id      = doc_id;
    e->version_ptr = version_ptr;
    return 0;
}

/* ===== MvccTxRegistry ===== */

void mvcc_registry_init(MvccTxRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

int mvcc_registry_add(MvccTxRegistry *reg, MvccTx *tx) {
    if (reg->count >= MVCC_REGISTRY_CAP) return -1;
    reg->tx_ids[reg->count] = tx->tx_id;
    reg->txs[reg->count]    = tx;
    reg->count++;
    return 0;
}

void mvcc_registry_snapshot(const MvccTxRegistry *reg,
                             uint64_t *out_ids, size_t *out_n, size_t max) {
    size_t n = (size_t)reg->count < max ? (size_t)reg->count : max;
    for (size_t i = 0; i < n; i++) {
        out_ids[i] = reg->tx_ids[i];
    }
    *out_n = n;
}

void mvcc_registry_remove(MvccTxRegistry *reg, uint64_t tx_id) {
    for (int i = 0; i < reg->count; i++) {
        if (reg->tx_ids[i] == tx_id) {
            /* Swap với phần tử cuối để xóa nhanh */
            reg->count--;
            reg->tx_ids[i] = reg->tx_ids[reg->count];
            reg->txs[i]    = reg->txs[reg->count];
            return;
        }
    }
}

MvccTx* mvcc_registry_find(const MvccTxRegistry *reg, uint64_t tx_id) {
    for (int i = 0; i < reg->count; i++) {
        if (reg->tx_ids[i] == tx_id) return reg->txs[i];
    }
    return NULL;
}

int mvcc_registry_is_active(const MvccTxRegistry *reg, uint64_t tx_id) {
    return mvcc_registry_find(reg, tx_id) != NULL;
}

/* ===== MvccCommittedTable ===== */

void mvcc_committed_table_init(MvccCommittedTable *tbl) {
    memset(tbl, 0, sizeof(*tbl));
}

void mvcc_committed_table_add(MvccCommittedTable *tbl,
                               uint64_t tx_id, uint64_t commit_ts, int aborted) {
    MvccTxRecord *e = &tbl->entries[tbl->head];
    e->tx_id     = tx_id;
    e->commit_ts = commit_ts;
    e->aborted   = aborted;
    tbl->head = (tbl->head + 1) % MVCC_COMMITTED_TABLE_SIZE;
    if (tbl->count < MVCC_COMMITTED_TABLE_SIZE) tbl->count++;
}

const MvccTxRecord* mvcc_committed_table_find(const MvccCommittedTable *tbl, uint64_t tx_id) {
    /* Linear scan — table nhỏ và đây là committed tx lookup ít khi cần */
    int limit = tbl->count < MVCC_COMMITTED_TABLE_SIZE ? tbl->count : MVCC_COMMITTED_TABLE_SIZE;
    for (int i = 0; i < limit; i++) {
        if (tbl->entries[i].tx_id == tx_id) return &tbl->entries[i];
    }
    return NULL;  /* Không tìm thấy → tx quá cũ, treat as committed */
}
