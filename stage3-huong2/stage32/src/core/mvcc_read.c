/* mvcc_read.c — MVCC Read Path + Visibility Check implementation
 *
 * Visibility algorithm (PostgreSQL-inspired Snapshot Isolation):
 *
 * Cho transaction T với begin_ts B đọc version V:
 *
 * Rule 1: Nếu V.created_tx là ABORTED tx → SKIP
 * Rule 2: Nếu V.created_tx == T.tx_id (own write) → VISIBLE (nếu chưa bị T delete)
 * Rule 3: Nếu V.created_tx nằm trong T.active_set (in-flight tại begin) → SKIP
 * Rule 4: Nếu V.created_ts > T.begin_ts (committed sau snapshot) → SKIP
 * Rule 5: Còn lại → version này committed trước snapshot → kiểm tra deleted_ts:
 *           - Nếu deleted_ts == 0 → VISIBLE
 *           - Nếu deleter là ABORTED → VISIBLE (delete reverted)
 *           - Nếu deleter là in-flight tại begin → VISIBLE (delete không thấy)
 *           - Nếu deleted_ts <= B và deleter committed trước snapshot → NOT_FOUND
 */
#include "mvcc_read.h"
#include "disk_db.h"
#include "doc_version.h"
#include "mvcc_tx.h"
#include "serializer.h"
#include "page.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== Internal: forward declarations ===== */
static Document* deserialize_doc_from_bytes(const uint8_t *buf, size_t size);

/* ===== mvcc_get_tx_state ===== */

int mvcc_get_tx_state(DiskDB *db, uint64_t tx_id, uint64_t *out_commit_ts) {
    if (out_commit_ts) *out_commit_ts = 0;

    /* Check active registry trước */
    if (mvcc_registry_is_active(&db->mvcc_registry, tx_id)) {
        return 1;  /* ACTIVE */
    }

    /* Check committed/aborted table */
    const MvccTxRecord *rec = mvcc_committed_table_find(&db->mvcc_committed, tx_id);
    if (rec) {
        if (rec->aborted) return 2;  /* ABORTED */
        if (out_commit_ts) *out_commit_ts = rec->commit_ts;
        return 0;  /* COMMITTED */
    }

    /* Không tìm thấy trong cả 2 → tx quá cũ, treat as committed.
     * Đây là safe vì GC chỉ xóa versions không ai cần nữa. */
    return -1;  /* UNKNOWN → treat as committed */
}

/* ===== Visibility check ===== */

VisibilityResult mvcc_check_visibility(DiskDB *db, MvccTx *viewer, const DocVersion *v) {
    uint64_t creator_tx = v->created_tx;

    /* ===== RULE 1: Creator bị abort → skip ===== */
    {
        int state = mvcc_get_tx_state(db, creator_tx, NULL);
        if (state == 2) {
            /* ABORTED — version này không bao giờ committed */
            return VIS_SKIP;
        }
    }

    /* ===== RULE 2: Own write (ta tự tạo) ===== */
    if (creator_tx == viewer->tx_id) {
        /* Kiểm tra nếu ta đã delete version này */
        if (v->deleted_ts != 0) {
            /* Deleted bởi ai? Cần kiểm tra creator của delete.
             * Trong MVCC append-only, delete tạo version MỚI với deleted_ts set.
             * Nếu version này có deleted_ts và được created bởi ta
             * → ta đã insert+delete trong cùng tx → NOT_FOUND. */
            return VIS_NOT_FOUND;
        }
        /* created_ts == 0 là bình thường (uncommitted own write) */
        return VIS_VISIBLE;
    }

    /* ===== RULE 3: Creator nằm trong active set (in-flight tại snapshot ta) ===== */
    for (size_t i = 0; i < viewer->n_active; i++) {
        if (viewer->active_set[i] == creator_tx) {
            return VIS_SKIP;
        }
    }

    /* ===== RULE 4: Creator committed sau snapshot ta ===== */
    {
        uint64_t creator_commit_ts = 0;
        int state = mvcc_get_tx_state(db, creator_tx, &creator_commit_ts);

        if (state == 1) {
            /* Vẫn ACTIVE — không nằm trong active_set của ta nhưng active.
             * Có thể là tx bắt đầu SAU ta begin (mới hơn) → SKIP. */
            return VIS_SKIP;
        }

        /* Committed (state == 0 hoặc -1/unknown) */
        if (state == 0 && creator_commit_ts > viewer->begin_ts) {
            /* Committed sau snapshot của ta → SKIP */
            return VIS_SKIP;
        }
        /* State -1 (unknown/old) → treat as committed trước snapshot, tiếp tục */
        /* State 0 với commit_ts <= begin_ts → tiếp tục */
    }

    /* ===== RULE 5: Version committed trước snapshot → kiểm tra deleted_ts ===== */
    if (v->deleted_ts == 0) {
        return VIS_VISIBLE;  /* Còn sống */
    }

    /* Version đã bị delete → kiểm tra deleter có visible với ta không.
     * Trong append-only model: deleted_ts được set bởi một delete operation.
     * Nếu deleted_ts > begin_ts → delete xảy ra sau snapshot → ta KHÔNG thấy delete
     *   → vẫn VISIBLE với ta.
     * Nếu deleted_ts <= begin_ts → cần check deleter tx state. */
    if (v->deleted_ts > viewer->begin_ts) {
        return VIS_VISIBLE;  /* Delete chưa visible trong snapshot ta */
    }

    /* deleted_ts <= begin_ts: delete xảy ra trước snapshot.
     * Nhưng cần verify deleter không phải in-flight hoặc aborted. */
    /* NOTE: Trong current implementation, deleted_ts được set = commit_ts của deleter.
     * Nếu deleter aborted, deleted_ts = 0 (ta reset về 0 khi abort).
     * Nên nếu deleted_ts != 0 và <= begin_ts → deleter đã committed → NOT_FOUND. */
    return VIS_NOT_FOUND;
}

/* ===== Page I/O helpers ===== */

int mvcc_page_read_version(DiskDB *db, uint64_t page_id, uint16_t offset,
                           DocVersion *v_out, uint8_t **data_out) {
    HugoPage page;
    if (pm_read_page(&db->pm, page_id, &page) != PG_OK) return -1;

    /* Bounds check */
    if (offset + DOC_VERSION_HDR_SIZE > HUGO_PAGE_DATA_SIZE) return -1;

    const uint8_t *src = page.data + offset;
    size_t avail = HUGO_PAGE_DATA_SIZE - offset;

    /* Peek header để biết data_size */
    if (doc_version_peek_header(src, avail, v_out) != 0) return -1;

    /* Allocate buffer cho data nếu cần */
    uint8_t *dbuf = NULL;
    if (v_out->data_size > 0) {
        dbuf = (uint8_t*)malloc(v_out->data_size);
        if (!dbuf) return -1;
    }

    DocVersion tmp;
    int r = doc_version_deserialize(src, avail, &tmp, dbuf, v_out->data_size);
    if (r < 0) {
        free(dbuf);
        return -1;
    }
    *v_out = tmp;
    if (data_out) {
        *data_out = dbuf;
    } else {
        free(dbuf);
    }
    return 0;
}

uint64_t mvcc_page_write_version(DiskDB *db, const DocVersion *v, const uint8_t *data) {
    size_t needed = doc_version_total_size(v->data_size);
    if (needed > HUGO_PAGE_DATA_SIZE) return VERSION_PTR_NULL;

    /* Alloc new page */
    uint64_t page_id;
    if (pm_alloc_page(&db->pm, &page_id) != PG_OK) return VERSION_PTR_NULL;

    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id   = (uint32_t)page_id;
    page.page_type = PAGE_TYPE_MVCC_VERSION;

    /* Serialize vào page.data bắt đầu từ offset 0 */
    int written = doc_version_serialize(v, data, page.data, HUGO_PAGE_DATA_SIZE);
    if (written < 0) return VERSION_PTR_NULL;

    if (pm_write_page(&db->pm, &page) != PG_OK) return VERSION_PTR_NULL;

    /* offset = 0 vì ta luôn ghi 1 version/page (đơn giản hoá) */
    return version_ptr_encode(page_id, 0);
}

int mvcc_page_set_created_ts(DiskDB *db, uint64_t version_ptr, uint64_t created_ts) {
    if (version_ptr == VERSION_PTR_NULL) return -1;
    uint64_t page_id = version_ptr_page(version_ptr);
    uint16_t offset  = version_ptr_offset(version_ptr);

    HugoPage page;
    if (pm_read_page(&db->pm, page_id, &page) != PG_OK) return -1;

    /* created_ts nằm ở bytes [8..15] trong DocVersion header */
    uint16_t field_off = offset + 8;
    if (field_off + 8 > HUGO_PAGE_DATA_SIZE) return -1;

    write_u64_be(page.data + field_off, created_ts);

    if (pm_write_page(&db->pm, &page) != PG_OK) return -1;
    return 0;
}

int mvcc_page_set_deleted_ts(DiskDB *db, uint64_t version_ptr, uint64_t deleted_ts) {
    if (version_ptr == VERSION_PTR_NULL) return -1;
    uint64_t page_id = version_ptr_page(version_ptr);
    uint16_t offset  = version_ptr_offset(version_ptr);

    HugoPage page;
    if (pm_read_page(&db->pm, page_id, &page) != PG_OK) return -1;

    /* deleted_ts nằm ở bytes [16..23] trong DocVersion header */
    uint16_t field_off = offset + 16;
    if (field_off + 8 > HUGO_PAGE_DATA_SIZE) return -1;

    write_u64_be(page.data + field_off, deleted_ts);

    if (pm_write_page(&db->pm, &page) != PG_OK) return -1;
    return 0;
}

/* ===== find_visible_version ===== */

int mvcc_find_visible_version(DiskDB *db, MvccTx *viewer,
                               uint64_t version_ptr,
                               DocVersion *v_out, uint8_t **data_out) {
    uint64_t ptr = version_ptr;
    int max_chain = 10000;  /* ngăn vòng lặp vô hạn nếu chain corrupt */

    while (ptr != VERSION_PTR_NULL && max_chain-- > 0) {
        DocVersion v;
        uint8_t *vdata = NULL;

        if (mvcc_page_read_version(db,
                                   version_ptr_page(ptr),
                                   version_ptr_offset(ptr),
                                   &v, &vdata) != 0) {
            return -1;  /* IO error */
        }

        VisibilityResult vis = mvcc_check_visibility(db, viewer, &v);

        if (vis == VIS_VISIBLE) {
            *v_out   = v;
            *data_out = vdata;
            return 0;
        }

        if (vis == VIS_NOT_FOUND) {
            free(vdata);
            return 1;  /* Document deleted theo visibility rules */
        }

        /* VIS_SKIP: walk sang version cũ hơn */
        uint64_t prev = v.prev_version_ptr;
        free(vdata);
        ptr = prev;
    }

    return 1;  /* Không tìm thấy version nào phù hợp */
}

/* ===== Document deserialization (copy logic từ disk_db.c) ===== */

static Document* deserialize_doc_from_bytes(const uint8_t *buf, size_t size) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + size;
    if (end - p < 2) return NULL;

    Document *doc = (Document*)calloc(1, sizeof(Document));
    if (!doc) return NULL;
    uint16_t n_pairs = read_u16_be(p); p += 2;

    KVPair *tail = NULL;
    for (uint16_t i = 0; i < n_pairs; i++) {
        if (end - p < 2) goto fail;
        uint16_t klen = read_u16_be(p); p += 2;
        if ((size_t)(end - p) < (size_t)klen + 1) goto fail;
        KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
        if (!kv) goto fail;
        if (klen >= sizeof(kv->key)) klen = (uint16_t)(sizeof(kv->key) - 1);
        memcpy(kv->key, p, klen); kv->key[klen] = '\0'; p += klen;
        kv->value.type = (ValType)*p++;

        if (kv->value.type == VAL_NUM) {
            if (end - p < 8) { free(kv); goto fail; }
            union { double d; uint64_t u; } u;
            u.u = read_u64_be(p); p += 8;
            kv->value.num = u.d;
        } else if (kv->value.type == VAL_STR) {
            if (end - p < 2) { free(kv); goto fail; }
            uint16_t slen = read_u16_be(p); p += 2;
            if ((size_t)(end - p) < slen) { free(kv); goto fail; }
            if (slen >= sizeof(kv->value.str)) slen = (uint16_t)(sizeof(kv->value.str) - 1);
            memcpy(kv->value.str, p, slen); kv->value.str[slen] = '\0';
            p += slen;
        } else if (kv->value.type == VAL_BOOL) {
            if (end - p < 1) { free(kv); goto fail; }
            kv->value.num = *p++ ? 1.0 : 0.0;
        }

        if (!doc->pairs) doc->pairs = tail = kv;
        else { tail->next = kv; tail = kv; }
        doc->count++;
    }
    return doc;

fail:
    {
        KVPair *kv = doc->pairs;
        while (kv) { KVPair *n = kv->next; free(kv); kv = n; }
        free(doc);
    }
    return NULL;
}

/* ===== mvcc_find_doc ===== */

Document* mvcc_find_doc(DiskDB *db, MvccTx *tx,
                        const char *coll_name, uint64_t doc_id,
                        int *err_out) {
    if (err_out) *err_out = 0;

    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) {
        if (err_out) *err_out = 1;
        return NULL;
    }

    if (doc_id == 0 || doc_id >= c->capacity) {
        if (err_out) *err_out = 1;
        return NULL;
    }

    /* Trong MVCC mode, doc_page_ids[doc_id] lưu VERSION_PTR thay vì plain page_id.
     * Convention: giá trị được set trong mvcc_insert_doc. */
    uint64_t version_ptr = c->doc_page_ids[doc_id];
    if (version_ptr == 0) {
        if (err_out) *err_out = 1;
        return NULL;
    }

    DocVersion v;
    uint8_t *vdata = NULL;
    int ret = mvcc_find_visible_version(db, tx, version_ptr, &v, &vdata);

    if (ret != 0) {
        /* ret = 1: not found/deleted; ret = -1: IO error */
        if (err_out) *err_out = (ret == -1) ? -1 : 1;
        return NULL;
    }

    /* Deserialize document từ version data */
    Document *doc = deserialize_doc_from_bytes(vdata, v.data_size);
    free(vdata);

    if (!doc) {
        if (err_out) *err_out = -1;
        return NULL;
    }
    return doc;
}

/* ===== mvcc_scan ===== */

int mvcc_scan(DiskDB *db, MvccTx *tx, const char *coll_name,
              mvcc_visit_fn fn, void *ctx) {
    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) return -1;

    for (uint64_t id = 1; id < c->capacity; id++) {
        uint64_t version_ptr = c->doc_page_ids[id];
        if (version_ptr == 0) continue;

        DocVersion v;
        uint8_t *vdata = NULL;
        int ret = mvcc_find_visible_version(db, tx, version_ptr, &v, &vdata);

        if (ret == 0 && vdata) {
            Document *doc = deserialize_doc_from_bytes(vdata, v.data_size);
            free(vdata);
            if (doc) {
                fn(id, doc, ctx);
                /* fn nhận ownership của doc, cần caller free trong fn */
            }
        } else {
            free(vdata);
        }
    }
    return 0;
}
