/* disk_db.h — Disk-backed database (Phase 8.a)
 *
 * Thay cho `HugoDatabase` RAM-only. Lưu documents vào page file qua PageManager.
 * Mỗi document chiếm 1 page (PAGE_TYPE_DOC). Index collection_name → doc_id
 * → page_id lưu trong header pages đầu file (META pages).
 *
 * Giới hạn MVP:
 *   - document ≤ 4070 bytes sau serialize (fit 1 page data area sau 7-byte
 *     disk_db doc header)
 *   - tối đa 64 collections
 *   - tối đa 100k docs / collection (in-memory index, persist khi close)
 *
 * File layout:
 *   page 0:      HugoHeader (đã có)
 *   page 1:      META page — collection catalog (names + metadata)
 *   page 2..N:   index pages — trỏ tới data pages cho mỗi collection
 *   page N+1..:  data pages (PAGE_TYPE_DOC), 1 page / document
 *
 * KHÔNG dùng B-tree ở MVP này — index là array trực tiếp trong META.
 * Phase 8.b sẽ thay bằng dbtree để support secondary indexes.
 */
#ifndef HUGO_DISK_DB_H
#define HUGO_DISK_DB_H

#include <stdint.h>
#include "page.h"
#include "wal.h"
#include "../query/ast.h"

#define DDB_MAX_COLLS        64
#define DDB_MAX_DOCS_PER_COLL 100000
#define PAGE_TYPE_DOC        0x06   /* data page chứa 1 document */
#define PAGE_TYPE_DOC_IDX    0x07   /* page chứa array doc_id → page_id */

/* Document bytes format trong page->data:
 *   [0..1]   n_pairs  u16 BE
 *   [2..]    pairs    sequentially:
 *              key_len  u16, key bytes
 *              val_type u8
 *              val:     (num: 8 bytes IEEE754 BE u64;
 *                        str: u16 len + bytes;
 *                        bool: 1 byte)
 */

/* Index metadata */
typedef struct {
    char     field[128];
    uint64_t btree_root_page;
} IndexMeta;

#define DDB_MAX_INDEXES 16

typedef struct {
    char       name[64];
    uint64_t   idx_page;
    uint64_t  *doc_page_ids;
    uint64_t   capacity;
    uint64_t   next_id;
    uint64_t   count;
    IndexMeta  indexes[DDB_MAX_INDEXES];
    int        n_indexes;
} DiskColl;

typedef struct {
    char         name[64];
    PageManager  pm;
    int          opened;
    DiskColl     colls[DDB_MAX_COLLS];
    int          n_colls;
    int          dirty;
    /* WAL (Phase 8.b) */
    Wal          wal;
    int          wal_enabled;
    char         log_path[512];
    /* Transaction state (Stage 2) */
    int          in_tx;
    uint64_t     current_tx_id;
} DiskDB;

/* Lifecycle */
int  ddb_create(DiskDB *db, const char *name, const char *path);
int  ddb_open  (DiskDB *db, const char *path);
int  ddb_close (DiskDB *db);      /* flush index + close pm */

/* Collections */
DiskColl* ddb_get_coll   (DiskDB *db, const char *name);
DiskColl* ddb_create_coll(DiskDB *db, const char *name);
int       ddb_drop_coll  (DiskDB *db, const char *name);
int       ddb_list_colls (const DiskDB *db, char names[][64], int max);

/* Documents — Document struct từ query/ast.h */
int       ddb_insert_doc(DiskDB *db, DiskColl *c, Document *doc, uint64_t *out_id);
Document* ddb_read_doc  (DiskDB *db, DiskColl *c, uint64_t id);   /* caller free */
int       ddb_update_doc(DiskDB *db, DiskColl *c, uint64_t id, Document *new_doc);
int       ddb_delete_doc(DiskDB *db, DiskColl *c, uint64_t id);

/* Iterate all docs — visit mỗi doc non-null, gọi fn */
typedef void (*ddb_visit_fn)(uint64_t id, Document *doc, void *ctx);
int       ddb_scan(DiskDB *db, DiskColl *c, ddb_visit_fn fn, void *ctx);

#endif
