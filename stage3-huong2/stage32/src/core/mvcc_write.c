/* mvcc_write.c — MVCC Write Path implementation */
#include "mvcc_write.h"
#include "mvcc_read.h"
#include "doc_version.h"
#include "serializer.h"
#include "page.h"
#include "collection.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== Internal: serialize document sang bytes ===== */
static int serialize_doc_to_buf(const Document *doc, uint8_t *buf, size_t max) {
    uint8_t *p = buf;
    const uint8_t *end = buf + max;

    if (end - p < 2) return -1;
    uint16_t n = (uint16_t)doc->count;
    write_u16_be(p, n); p += 2;

    for (const KVPair *kv = doc->pairs; kv; kv = kv->next) {
        uint16_t klen = (uint16_t)strlen(kv->key);
        if ((size_t)(end - p) < (size_t)(2 + klen + 1)) return -1;
        write_u16_be(p, klen); p += 2;
        memcpy(p, kv->key, klen); p += klen;
        *p++ = (uint8_t)kv->value.type;

        if (kv->value.type == VAL_NUM) {
            if (end - p < 8) return -1;
            union { double d; uint64_t u; } u;
            u.d = kv->value.num;
            write_u64_be(p, u.u); p += 8;
        } else if (kv->value.type == VAL_STR) {
            uint16_t slen = (uint16_t)strlen(kv->value.str);
            if ((size_t)(end - p) < (size_t)(2 + slen)) return -1;
            write_u16_be(p, slen); p += 2;
            memcpy(p, kv->value.str, slen); p += slen;
        } else if (kv->value.type == VAL_BOOL) {
            if (end - p < 1) return -1;
            *p++ = (uint8_t)(kv->value.num != 0.0);
        }
    }
    return (int)(p - buf);
}

/* ===== Write-write conflict check ===== */
/* Trả về 0 = no conflict, MVCC_ERR_CONFLICT = conflict */
static int check_write_conflict(DiskDB *db, MvccTx *tx, uint64_t latest_version_ptr) {
    if (latest_version_ptr == VERSION_PTR_NULL) return 0;

    DocVersion latest;
    if (mvcc_page_read_version(db,
                               version_ptr_page(latest_version_ptr),
                               version_ptr_offset(latest_version_ptr),
                               &latest, NULL) != 0) {
        return 0;  /* Không đọc được → assume no conflict */
    }

    uint64_t creator = latest.created_tx;
    if (creator == tx->tx_id) return 0;  /* Chính mình → OK */

    /* Check creator state */
    uint64_t creator_commit_ts = 0;
    int state = mvcc_get_tx_state(db, creator, &creator_commit_ts);

    if (state == 2) return 0;  /* Creator aborted → no conflict */
    if (state == 1) return MVCC_ERR_CONFLICT;  /* Creator đang active → concurrent write */

    /* Creator committed */
    if (creator_commit_ts > tx->begin_ts) {
        return MVCC_ERR_CONFLICT;  /* Committed sau snapshot ta → conflict */
    }
    return 0;
}

/* ===== mvcc_insert_doc ===== */

int mvcc_insert_doc(DiskDB *db, MvccTx *tx, const char *coll_name,
                    Document *doc, uint64_t *out_id) {
    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) {
        c = ddb_create_coll(db, coll_name);
        if (!c) return MVCC_ERR_IO;
    }

    /* Auto-assign id */
    uint64_t id = c->next_id;

    /* Auto-add "id" field nếu chưa có */
    Value chk;
    if (doc_get_field(doc, "id", &chk) != 0) {
        Value v; memset(&v, 0, sizeof(v));
        v.type = VAL_NUM; v.num = (double)id;
        doc_set_field(doc, "id", v);
    }

    /* Serialize document */
    uint8_t doc_buf[HUGO_PAGE_DATA_SIZE];
    int doc_len = serialize_doc_to_buf(doc, doc_buf, sizeof(doc_buf));
    if (doc_len < 0) return MVCC_ERR_TOOLARGE;

    /* Tạo DocVersion */
    DocVersion v;
    memset(&v, 0, sizeof(v));
    v.version_id       = ts_oracle_next(&db->mvcc_oracle);
    v.created_ts       = 0;            /* uncommitted — set khi commit */
    v.deleted_ts       = 0;
    v.created_tx       = tx->tx_id;
    v.prev_version_ptr = VERSION_PTR_NULL;
    v.data_size        = (uint32_t)doc_len;

    /* Ghi version vào page */
    uint64_t version_ptr = mvcc_page_write_version(db, &v, doc_buf);
    if (version_ptr == VERSION_PTR_NULL) return MVCC_ERR_IO;

    /* Grow doc_page_ids array nếu cần (inline của coll_grow) */
    if (c->capacity <= id) {
        uint64_t new_cap = c->capacity ? c->capacity : 64;
        while (new_cap <= id) new_cap *= 2;
        uint64_t *nd = (uint64_t*)realloc(c->doc_page_ids, new_cap * sizeof(uint64_t));
        if (!nd) return MVCC_ERR_NOMEM;
        memset(nd + c->capacity, 0, (new_cap - c->capacity) * sizeof(uint64_t));
        c->doc_page_ids = nd;
        c->capacity = new_cap;
    }

    c->doc_page_ids[id] = version_ptr;  /* Lưu VERSION_PTR thay vì page_id raw */
    c->next_id++;
    c->count++;
    db->dirty = 1;

    /* WAL: log version creation TRƯỚC khi return (Phase 5) */
    if (db->wal_enabled) {
        wal_log_mvcc_version(&db->wal, tx->tx_id, id, version_ptr, coll_name);
        wal_sync(&db->wal);
    }

    /* Track trong write set */
    if (mvcc_tx_track_write(tx, coll_name, id, version_ptr) != 0) {
        return MVCC_ERR_NOMEM;
    }

    if (out_id) *out_id = id;
    return MVCC_OK;
}

/* ===== mvcc_update_doc ===== */

int mvcc_update_doc(DiskDB *db, MvccTx *tx, const char *coll_name,
                    uint64_t doc_id, Document *new_doc) {
    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) return MVCC_ERR_NOTFOUND;
    if (doc_id == 0 || doc_id >= c->capacity) return MVCC_ERR_NOTFOUND;

    uint64_t old_version_ptr = c->doc_page_ids[doc_id];
    if (old_version_ptr == 0) return MVCC_ERR_NOTFOUND;

    /* Write-write conflict check */
    int conflict = check_write_conflict(db, tx, old_version_ptr);
    if (conflict != 0) {
        tx->state = MVCC_TX_ABORTED;  /* Mark tx aborted */
        return MVCC_ERR_CONFLICT;
    }

    /* Serialize new document */
    uint8_t doc_buf[HUGO_PAGE_DATA_SIZE];
    int doc_len = serialize_doc_to_buf(new_doc, doc_buf, sizeof(doc_buf));
    if (doc_len < 0) return MVCC_ERR_TOOLARGE;

    /* Tạo DocVersion MỚI trỏ về version cũ */
    DocVersion v;
    memset(&v, 0, sizeof(v));
    v.version_id       = ts_oracle_next(&db->mvcc_oracle);
    v.created_ts       = 0;
    v.deleted_ts       = 0;
    v.created_tx       = tx->tx_id;
    v.prev_version_ptr = old_version_ptr;  /* Chain pointer về version trước */
    v.data_size        = (uint32_t)doc_len;

    uint64_t new_version_ptr = mvcc_page_write_version(db, &v, doc_buf);
    if (new_version_ptr == VERSION_PTR_NULL) return MVCC_ERR_IO;

    /* Update doc_page_ids để trỏ tới latest version */
    c->doc_page_ids[doc_id] = new_version_ptr;
    db->dirty = 1;

    /* WAL: log version creation */
    if (db->wal_enabled) {
        wal_log_mvcc_version(&db->wal, tx->tx_id, doc_id, new_version_ptr, coll_name);
        wal_sync(&db->wal);
    }

    /* Track write */
    if (mvcc_tx_track_write(tx, coll_name, doc_id, new_version_ptr) != 0) {
        return MVCC_ERR_NOMEM;
    }

    return MVCC_OK;
}

/* ===== mvcc_delete_doc ===== */

int mvcc_delete_doc(DiskDB *db, MvccTx *tx, const char *coll_name, uint64_t doc_id) {
    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) return MVCC_ERR_NOTFOUND;
    if (doc_id == 0 || doc_id >= c->capacity) return MVCC_ERR_NOTFOUND;

    uint64_t current_ptr = c->doc_page_ids[doc_id];
    if (current_ptr == 0) return MVCC_ERR_NOTFOUND;

    /* Conflict check */
    int conflict = check_write_conflict(db, tx, current_ptr);
    if (conflict != 0) {
        tx->state = MVCC_TX_ABORTED;
        return MVCC_ERR_CONFLICT;
    }

    /* Tạo "tombstone" version: data_size = 0, deleted_ts = UINT64_MAX (placeholder,
     * sẽ được set đúng khi commit). */
    DocVersion v;
    memset(&v, 0, sizeof(v));
    v.version_id       = ts_oracle_next(&db->mvcc_oracle);
    v.created_ts       = 0;
    v.deleted_ts       = UINT64_MAX;  /* placeholder — set = commit_ts khi commit */
    v.created_tx       = tx->tx_id;
    v.prev_version_ptr = current_ptr;
    v.data_size        = 0;  /* tombstone: không có data */

    uint64_t tomb_ptr = mvcc_page_write_version(db, &v, NULL);
    if (tomb_ptr == VERSION_PTR_NULL) return MVCC_ERR_IO;

    /* Update latest pointer */
    c->doc_page_ids[doc_id] = tomb_ptr;
    db->dirty = 1;

    /* WAL: log tombstone version */
    if (db->wal_enabled) {
        /* Encode doc_id với flag để distinguish từ regular version */
        wal_log_mvcc_version(&db->wal, tx->tx_id,
                             doc_id | ((uint64_t)1 << 63), tomb_ptr, coll_name);
        wal_sync(&db->wal);
    }

    /* Track write — special convention: doc_id | HIGH_BIT đánh dấu là delete */
    if (mvcc_tx_track_write(tx, coll_name, doc_id | ((uint64_t)1 << 63), tomb_ptr) != 0) {
        return MVCC_ERR_NOMEM;
    }

    return MVCC_OK;
}

/* ===== mvcc_commit_tx ===== */

int mvcc_commit_tx(DiskDB *db, MvccTx *tx) {
    /* Lấy commit timestamp */
    uint64_t commit_ts = ts_oracle_next(&db->mvcc_oracle);
    tx->commit_ts = commit_ts;
    tx->state     = MVCC_TX_COMMITTED;

    /* Set created_ts / finalize deleted_ts cho tất cả versions trong write_set */
    for (size_t i = 0; i < tx->n_writes; i++) {
        MvccWriteEntry *e = &tx->write_set[i];
        uint64_t actual_doc_id = e->doc_id & ~((uint64_t)1 << 63);
        int is_delete = (e->doc_id >> 63) & 1;

        if (is_delete) {
            /* Finalize tombstone: set deleted_ts = commit_ts */
            mvcc_page_set_deleted_ts(db, e->version_ptr, commit_ts);
            /* Cũng set created_ts cho tombstone version */
            mvcc_page_set_created_ts(db, e->version_ptr, commit_ts);
        } else {
            /* INSERT / UPDATE: set created_ts */
            mvcc_page_set_created_ts(db, e->version_ptr, commit_ts);
        }
        (void)actual_doc_id;
    }

    /* Log WAL commit */
    if (db->wal_enabled) {
        wal_log_mvcc_commit(&db->wal, tx->tx_id, commit_ts);
        wal_sync(&db->wal);
    }

    /* Cleanup registry */
    mvcc_registry_remove(&db->mvcc_registry, tx->tx_id);
    mvcc_committed_table_add(&db->mvcc_committed, tx->tx_id, commit_ts, 0);

    return MVCC_OK;
}

/* ===== mvcc_abort_tx ===== */

int mvcc_abort_tx(DiskDB *db, MvccTx *tx) {
    tx->state = MVCC_TX_ABORTED;

    /* Undo tất cả writes */
    for (size_t i = 0; i < tx->n_writes; i++) {
        MvccWriteEntry *e = &tx->write_set[i];
        uint64_t actual_doc_id = e->doc_id & ~((uint64_t)1 << 63);
        int is_delete = (e->doc_id >> 63) & 1;

        DiskColl *c = ddb_get_coll(db, e->coll_name);
        if (!c) continue;

        if (is_delete) {
            /* Undo delete: restore doc_page_ids về version trước tombstone */
            DocVersion tomb;
            if (mvcc_page_read_version(db,
                                       version_ptr_page(e->version_ptr),
                                       version_ptr_offset(e->version_ptr),
                                       &tomb, NULL) == 0) {
                c->doc_page_ids[actual_doc_id] = tomb.prev_version_ptr;
            }
            /* Tombstone page vẫn còn nhưng sẽ bị GC dọn sau */
        } else {
            /* Undo insert/update: đọc prev_version_ptr và restore */
            DocVersion v;
            if (mvcc_page_read_version(db,
                                       version_ptr_page(e->version_ptr),
                                       version_ptr_offset(e->version_ptr),
                                       &v, NULL) == 0) {
                if (v.prev_version_ptr == VERSION_PTR_NULL) {
                    /* Là INSERT mới → xóa doc khỏi index */
                    c->doc_page_ids[actual_doc_id] = 0;
                    if (c->count > 0) c->count--;
                } else {
                    /* Là UPDATE → restore về version trước */
                    c->doc_page_ids[actual_doc_id] = v.prev_version_ptr;
                }
            }
        }
    }

    db->dirty = 1;

    /* Log WAL abort */
    if (db->wal_enabled) {
        wal_log_abort(&db->wal, tx->tx_id);
        wal_sync(&db->wal);
    }

    /* Cleanup registry */
    mvcc_registry_remove(&db->mvcc_registry, tx->tx_id);
    mvcc_committed_table_add(&db->mvcc_committed, tx->tx_id, 0, 1);

    return MVCC_OK;
}
