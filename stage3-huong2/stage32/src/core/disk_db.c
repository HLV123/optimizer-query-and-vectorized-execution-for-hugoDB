/* disk_db.c — Disk-backed database implementation */
#include "disk_db.h"
#include "collection.h"
#include "serializer.h"
#include "mvcc_read.h"   /* mvcc helpers */
#include "mvcc_recovery.h" /* MVCC WAL recovery */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Derive log path: "xxx.hugo" → "xxx.hugolog" */
static void derive_log_path(const char *db_path, char *log_path, size_t max) {
    size_t n = strlen(db_path);
    /* Nếu kết thúc ".hugo" thì thay bằng ".hugolog"; nếu không append ".hugolog" */
    if (n >= 5 && strcmp(db_path + n - 5, ".hugo") == 0) {
        snprintf(log_path, max, "%.*slog", (int)n, db_path);
    } else {
        snprintf(log_path, max, "%s.hugolog", db_path);
    }
}

/* ===== Document serialization ===== */

/* Write document vào buffer (fit trong page data area 4077 bytes).
 * Trả về số bytes đã ghi, -1 nếu overflow. */
static int serialize_doc(const Document *doc, uint8_t *buf, size_t max) {
    uint8_t *p = buf;
    uint8_t *end = buf + max;
    if (end - p < 2) return -1;

    uint16_t n_pairs = (uint16_t)doc->count;
    write_u16_be(p, n_pairs); p += 2;

    for (const KVPair *kv = doc->pairs; kv; kv = kv->next) {
        uint16_t klen = (uint16_t)strlen(kv->key);
        if ((size_t)(end - p) < 2 + klen + 1) return -1;
        write_u16_be(p, klen); p += 2;
        memcpy(p, kv->key, klen); p += klen;

        *p++ = (uint8_t)kv->value.type;

        if (kv->value.type == VAL_NUM) {
            if ((size_t)(end - p) < 8) return -1;
            union { double d; uint64_t u; } u;
            u.d = kv->value.num;
            write_u64_be(p, u.u); p += 8;
        } else if (kv->value.type == VAL_STR) {
            uint16_t slen = (uint16_t)strlen(kv->value.str);
            if ((size_t)(end - p) < 2 + slen) return -1;
            write_u16_be(p, slen); p += 2;
            memcpy(p, kv->value.str, slen); p += slen;
        } else if (kv->value.type == VAL_BOOL) {
            if (end - p < 1) return -1;
            *p++ = (uint8_t)(kv->value.num != 0);
        }
    }
    return (int)(p - buf);
}

static Document* deserialize_doc(const uint8_t *buf, size_t size) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + size;
    if (end - p < 2) return NULL;

    Document *doc = (Document*)calloc(1, sizeof(Document));
    uint16_t n_pairs = read_u16_be(p); p += 2;

    KVPair *tail = NULL;
    for (uint16_t i = 0; i < n_pairs; i++) {
        if (end - p < 2) goto fail;
        uint16_t klen = read_u16_be(p); p += 2;
        if ((size_t)(end - p) < klen + 1) goto fail;
        KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
        if (klen >= sizeof(kv->key)) klen = sizeof(kv->key) - 1;
        memcpy(kv->key, p, klen); kv->key[klen] = 0; p += klen;

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
            if (slen >= sizeof(kv->value.str)) slen = sizeof(kv->value.str) - 1;
            memcpy(kv->value.str, p, slen); kv->value.str[slen] = 0;
            p += slen;
        } else if (kv->value.type == VAL_BOOL) {
            if (end - p < 1) { free(kv); goto fail; }
            kv->value.num = *p++ ? 1 : 0;
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

/* ===== META page (page 1) layout =====
 *   [0..1]   n_collections u16 BE
 *   [2..]    for each: name_len u16, name bytes, idx_page u64,
 *            next_id u64, count u64
 */
static int load_meta(DiskDB *db) {
    if (db->pm.hdr.page_count < 2) return 0;
    HugoPage page;
    if (pm_read_page(&db->pm, 1, &page) != PG_OK) return 0;

    const uint8_t *p = page.data;
    const uint8_t *end = page.data + HUGO_PAGE_DATA_SIZE;
    if (end - p < 2) return 0;

    uint16_t n = read_u16_be(p); p += 2;
    for (uint16_t i = 0; i < n && i < DDB_MAX_COLLS; i++) {
        if (end - p < 2) return -1;
        uint16_t nlen = read_u16_be(p); p += 2;
        if ((size_t)(end - p) < nlen + 24) return -1;
        DiskColl *c = &db->colls[db->n_colls++];
        memset(c, 0, sizeof(*c));
        if (nlen >= sizeof(c->name)) nlen = sizeof(c->name) - 1;
        memcpy(c->name, p, nlen); c->name[nlen] = 0; p += nlen;
        c->idx_page = read_u64_be(p); p += 8;
        c->next_id  = read_u64_be(p); p += 8;
        c->count    = read_u64_be(p); p += 8;

        /* Read index metadata */
        if (end - p >= 2) {
            uint16_t n_idx = read_u16_be(p); p += 2;
            c->n_indexes = 0;
            for (uint16_t j = 0; j < n_idx && j < DDB_MAX_INDEXES; j++) {
                if (end - p < 2) break;
                uint16_t flen = read_u16_be(p); p += 2;
                if ((size_t)(end - p) < flen + 8) break;
                IndexMeta *im = &c->indexes[c->n_indexes++];
                memset(im, 0, sizeof(*im));
                if (flen >= sizeof(im->field)) flen = sizeof(im->field)-1;
                memcpy(im->field, p, flen); im->field[flen] = 0; p += flen;
                im->btree_root_page = read_u64_be(p); p += 8;
            }
        }

        /* Load index page: doc_page_ids array */
        c->capacity = c->next_id > 64 ? c->next_id + 64 : 64;
        c->doc_page_ids = (uint64_t*)calloc(c->capacity, sizeof(uint64_t));
        if (c->idx_page != 0) {
            HugoPage idx;
            if (pm_read_page(&db->pm, c->idx_page, &idx) == PG_OK) {
                const uint8_t *ip = idx.data;
                uint32_t n_entries = read_u32_be(ip); ip += 4;
                for (uint32_t j = 0; j < n_entries; j++) {
                    uint64_t did = read_u64_be(ip); ip += 8;
                    uint64_t pid = read_u64_be(ip); ip += 8;
                    if (did > 0 && did < c->capacity) c->doc_page_ids[did] = pid;
                }
            }
        }
    }
    return 0;
}

static int save_meta(DiskDB *db) {
    HugoPage page;
    memset(&page, 0, sizeof(page));

    /* Allocate META page nếu chưa có */
    uint64_t meta_pid = 1;
    if (db->pm.hdr.page_count < 2) {
        uint64_t pid;
        pm_alloc_page(&db->pm, &pid);  /* = 1 */
    }
    page.page_id = (uint32_t)meta_pid;
    page.page_type = PAGE_TYPE_META;

    uint8_t *p = page.data;
    uint8_t *end = page.data + HUGO_PAGE_DATA_SIZE;
    write_u16_be(p, (uint16_t)db->n_colls); p += 2;

    for (int i = 0; i < db->n_colls; i++) {
        DiskColl *c = &db->colls[i];
        if (c->idx_page == 0) pm_alloc_page(&db->pm, &c->idx_page);
        /* Build index page content */
        HugoPage idx; memset(&idx, 0, sizeof(idx));
        idx.page_id = (uint32_t)c->idx_page;
        idx.page_type = PAGE_TYPE_DOC_IDX;
        uint8_t *ip = idx.data; ip += 4; /* skip n_entries placeholder */
        uint32_t n_entries = 0;
        uint8_t *idx_end = idx.data + HUGO_PAGE_DATA_SIZE;
        for (uint64_t did = 1; did < c->capacity && did < c->next_id; did++) {
            if (c->doc_page_ids[did] == 0) continue;
            if ((size_t)(idx_end - ip) < 16) break;
            write_u64_be(ip, did); ip += 8;
            write_u64_be(ip, c->doc_page_ids[did]); ip += 8;
            n_entries++;
        }
        write_u32_be(idx.data, n_entries);
        pm_write_page(&db->pm, &idx);

        /* Write collection entry in META */
        uint16_t nlen = (uint16_t)strlen(c->name);
        if ((size_t)(end - p) < 2 + nlen + 24 + 2) break;
        write_u16_be(p, nlen); p += 2;
        memcpy(p, c->name, nlen); p += nlen;
        write_u64_be(p, c->idx_page); p += 8;
        write_u64_be(p, c->next_id);  p += 8;
        write_u64_be(p, c->count);    p += 8;

        /* Write index metadata */
        write_u16_be(p, (uint16_t)c->n_indexes); p += 2;
        for (int j = 0; j < c->n_indexes; j++) {
            uint16_t flen = (uint16_t)strlen(c->indexes[j].field);
            if ((size_t)(end - p) < 2 + flen + 8) break;
            write_u16_be(p, flen); p += 2;
            memcpy(p, c->indexes[j].field, flen); p += flen;
            write_u64_be(p, c->indexes[j].btree_root_page); p += 8;
        }
    }

    return pm_write_page(&db->pm, &page) == PG_OK ? 0 : -1;
}

/* ===== Lifecycle ===== */
int ddb_create(DiskDB *db, const char *name, const char *path) {
    memset(db, 0, sizeof(*db));
    strncpy(db->name, name, sizeof(db->name)-1);
    if (pm_create(&db->pm, path) != PG_OK) return -1;
    db->opened = 1;
    /* Alloc META page (page 1) */
    uint64_t pid;
    pm_alloc_page(&db->pm, &pid);  /* = 1 */
    /* Save empty meta */
    save_meta(db);
    pm_flush_header(&db->pm);

    /* Open WAL */
    derive_log_path(path, db->log_path, sizeof(db->log_path));
    /* Xóa log cũ nếu có (DB mới) */
    remove(db->log_path);
    if (wal_open(&db->wal, db->log_path) == WAL_OK) {
        db->wal_enabled = 1;
    }

    /* Init MVCC infrastructure (mode mặc định là 2PL) */
    db->mode = HUGO_MODE_2PL;
    ts_oracle_init(&db->mvcc_oracle, 1);
    mvcc_registry_init(&db->mvcc_registry);
    mvcc_committed_table_init(&db->mvcc_committed);

    return 0;
}

int ddb_open(DiskDB *db, const char *path) {
    memset(db, 0, sizeof(*db));
    if (pm_open(&db->pm, path) != PG_OK) return -1;
    db->opened = 1;

    /* Open WAL và chạy recovery nếu có record */
    derive_log_path(path, db->log_path, sizeof(db->log_path));
    if (wal_open(&db->wal, db->log_path) == WAL_OK) {
        db->wal_enabled = 1;
        /* Recovery: REDO + UNDO nếu log có record */
        if (db->wal.size > 0) {
            printf("(found WAL, running recovery...)\n");
            wal_recover(&db->wal, &db->pm);
            /* Clear log sau khi recovery thành công */
            wal_truncate(&db->wal);
        }
    }

    if (load_meta(db) != 0) {
        pm_close(&db->pm);
        if (db->wal_enabled) wal_close(&db->wal);
        return -1;
    }

    /* Init MVCC infrastructure */
    db->mode = HUGO_MODE_2PL;
    ts_oracle_init(&db->mvcc_oracle, 1);
    mvcc_registry_init(&db->mvcc_registry);
    mvcc_committed_table_init(&db->mvcc_committed);

    /* Advance oracle dựa trên WAL next_tx_id (đã persist qua WAL scan khi open).
     * Điều này đảm bảo timestamps không bị reuse dù WAL đã truncate. */
    if (db->wal_enabled) {
        ts_oracle_advance(&db->mvcc_oracle, db->wal.next_tx_id * 2 + 10);
    }

    /* MVCC recovery: rebuild logical state từ WAL (sau physical REDO/UNDO) */
    if (db->wal_enabled) {
        mvcc_recover(db);
    }

    return 0;
}

int ddb_open_mode(DiskDB *db, const char *path, IsolationMode mode) {
    int r = ddb_open(db, path);
    if (r != 0) return r;
    db->mode = mode;
    return 0;
}

int ddb_close(DiskDB *db) {
    if (!db->opened) return 0;
    if (db->dirty) {
        save_meta(db);
    }
    /* Free in-RAM index arrays */
    for (int i = 0; i < db->n_colls; i++) {
        free(db->colls[i].doc_page_ids);
        db->colls[i].doc_page_ids = NULL;
    }
    /* Clean close → truncate WAL (mọi thứ đã được persist qua meta) */
    if (db->wal_enabled) {
        wal_truncate(&db->wal);
        wal_close(&db->wal);
        db->wal_enabled = 0;
    }
    int rc = pm_close(&db->pm);
    db->opened = 0;
    return rc == PG_OK ? 0 : -1;
}

/* ===== Collection operations ===== */
DiskColl* ddb_get_coll(DiskDB *db, const char *name) {
    for (int i = 0; i < db->n_colls; i++)
        if (strcmp(db->colls[i].name, name) == 0) return &db->colls[i];
    return NULL;
}

DiskColl* ddb_create_coll(DiskDB *db, const char *name) {
    if (ddb_get_coll(db, name)) return NULL;  /* duplicate */
    if (db->n_colls >= DDB_MAX_COLLS) return NULL;
    DiskColl *c = &db->colls[db->n_colls++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name)-1);
    c->capacity = 64;
    c->doc_page_ids = (uint64_t*)calloc(c->capacity, sizeof(uint64_t));
    c->next_id = 1;
    c->count = 0;
    c->idx_page = 0;  /* allocated lazily in save_meta */
    db->dirty = 1;
    return c;
}

int ddb_drop_coll(DiskDB *db, const char *name) {
    for (int i = 0; i < db->n_colls; i++) {
        if (strcmp(db->colls[i].name, name) == 0) {
            /* Free in-RAM index (data pages orphan — free list sẽ xử lý) */
            free(db->colls[i].doc_page_ids);
            for (int j = i; j < db->n_colls - 1; j++)
                db->colls[j] = db->colls[j+1];
            db->n_colls--;
            db->dirty = 1;
            return 0;
        }
    }
    return -1;
}

int ddb_list_colls(const DiskDB *db, char names[][64], int max) {
    int n = (db->n_colls < max) ? db->n_colls : max;
    for (int i = 0; i < n; i++) strncpy(names[i], db->colls[i].name, 64);
    return db->n_colls;
}

/* ===== Document operations ===== */
static void coll_grow(DiskColl *c, uint64_t min_cap) {
    if (c->capacity >= min_cap) return;
    uint64_t new_cap = c->capacity;
    while (new_cap < min_cap) new_cap *= 2;
    uint64_t *nd = (uint64_t*)realloc(c->doc_page_ids, new_cap * sizeof(uint64_t));
    if (!nd) return;
    memset(nd + c->capacity, 0, (new_cap - c->capacity) * sizeof(uint64_t));
    c->doc_page_ids = nd;
    c->capacity = new_cap;
}

int ddb_insert_doc(DiskDB *db, DiskColl *c, Document *doc, uint64_t *out_id) {
    if (!c || !doc) return -1;

    /* Auto-add "id" field nếu chưa có */
    uint64_t id = c->next_id;
    Value chk;
    if (doc_get_field(doc, "id", &chk) != 0) {
        Value v; memset(&v, 0, sizeof(v));
        v.type = VAL_NUM; v.num = (double)id;
        doc_set_field(doc, "id", v);
    }

    /* Serialize doc */
    uint8_t buf[HUGO_PAGE_DATA_SIZE];
    int n = serialize_doc(doc, buf, sizeof(buf));
    if (n < 0) return -2;  /* too large */

    /* Alloc page */
    uint64_t pid;
    if (pm_alloc_page(&db->pm, &pid) != PG_OK) return -3;

    /* WAL: log before/after BEFORE writing page */
    if (db->wal_enabled) {
        /* Page mới alloc → before_image = toàn 0 */
        uint8_t before[HUGO_PAGE_DATA_SIZE];
        memset(before, 0, sizeof(before));
        uint8_t after[HUGO_PAGE_DATA_SIZE];
        memset(after, 0, sizeof(after));
        memcpy(after, buf, (size_t)n);

        uint64_t tx = wal_new_tx_id(&db->wal);
        wal_log_begin(&db->wal, tx);
        wal_log_update(&db->wal, tx, pid, 0, before, after, HUGO_PAGE_DATA_SIZE);
        wal_log_commit(&db->wal, tx);
        wal_sync(&db->wal);
    }

    /* Write page */
    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id = (uint32_t)pid;
    page.page_type = PAGE_TYPE_DOC;
    memcpy(page.data, buf, (size_t)n);
    if (pm_write_page(&db->pm, &page) != PG_OK) return -4;

    /* Update index */
    coll_grow(c, id + 1);
    c->doc_page_ids[id] = pid;
    c->next_id++;
    c->count++;
    db->dirty = 1;

    if (out_id) *out_id = id;
    return 0;
}

Document* ddb_read_doc(DiskDB *db, DiskColl *c, uint64_t id) {
    if (!c || id == 0 || id >= c->capacity) return NULL;
    uint64_t pid = c->doc_page_ids[id];
    if (pid == 0) return NULL;
    HugoPage page;
    if (pm_read_page(&db->pm, pid, &page) != PG_OK) return NULL;
    if (page.page_type != PAGE_TYPE_DOC) return NULL;
    /* Deserialize — dùng full data area (extra bytes ignored via n_pairs count) */
    return deserialize_doc(page.data, HUGO_PAGE_DATA_SIZE);
}

int ddb_update_doc(DiskDB *db, DiskColl *c, uint64_t id, Document *new_doc) {
    if (!c || id == 0 || id >= c->capacity) return -1;
    uint64_t pid = c->doc_page_ids[id];
    if (pid == 0) return -1;

    uint8_t buf[HUGO_PAGE_DATA_SIZE];
    int n = serialize_doc(new_doc, buf, sizeof(buf));
    if (n < 0) return -2;

    /* WAL: read old page data (before_image) */
    if (db->wal_enabled) {
        HugoPage old_page;
        uint8_t before[HUGO_PAGE_DATA_SIZE];
        if (pm_read_page(&db->pm, pid, &old_page) == PG_OK) {
            memcpy(before, old_page.data, HUGO_PAGE_DATA_SIZE);
        } else {
            memset(before, 0, HUGO_PAGE_DATA_SIZE);
        }
        uint8_t after[HUGO_PAGE_DATA_SIZE];
        memset(after, 0, sizeof(after));
        memcpy(after, buf, (size_t)n);

        uint64_t tx = wal_new_tx_id(&db->wal);
        wal_log_begin(&db->wal, tx);
        wal_log_update(&db->wal, tx, pid, 0, before, after, HUGO_PAGE_DATA_SIZE);
        wal_log_commit(&db->wal, tx);
        wal_sync(&db->wal);
    }

    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id = (uint32_t)pid;
    page.page_type = PAGE_TYPE_DOC;
    memcpy(page.data, buf, (size_t)n);
    if (pm_write_page(&db->pm, &page) != PG_OK) return -4;
    db->dirty = 1;
    return 0;
}

int ddb_delete_doc(DiskDB *db, DiskColl *c, uint64_t id) {
    if (!c || id == 0 || id >= c->capacity) return -1;
    if (c->doc_page_ids[id] == 0) return -1;
    /* Mark page free (MVP: chỉ zero index, không reuse page) */
    c->doc_page_ids[id] = 0;
    c->count--;
    db->dirty = 1;
    return 0;
}

int ddb_scan(DiskDB *db, DiskColl *c, ddb_visit_fn fn, void *ctx) {
    if (!c) return -1;
    int visited = 0;
    for (uint64_t id = 1; id < c->capacity && id < c->next_id; id++) {
        if (c->doc_page_ids[id] == 0) continue;
        Document *d = ddb_read_doc(db, c, id);
        if (!d) continue;
        fn(id, d, ctx);
        /* fn là owner của d — nếu cần keep, clone */
        doc_free(d);
        visited++;
    }
    return visited;
}
