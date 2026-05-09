/* dbtree.h — Disk-backed B-tree (Phase 2.b)
 *
 * Mỗi node = 1 page (4096 bytes) trong file .hugo.
 * Node trỏ đến nhau qua page_id (uint64), không qua pointer RAM.
 *
 * Disk node format trong page->data (4077 bytes):
 *   [0]      is_leaf      u8
 *   [1..2]   num_keys     u16 BE
 *   [3..4]   pad          (= 0)
 *   [5..]    keys[]       u64 BE × num_keys      (8 × num_keys bytes)
 *   [...]    values[]     u64 BE × num_keys      (8 × num_keys bytes)
 *   [...]    children[]   u64 BE × (num_keys+1)  (8 × (n+1), chỉ nếu !leaf)
 *
 * Root page_id được lưu trong DB header (hdr.root_page).
 * Khi root_page = 0 → cây rỗng.
 *
 * MVP version: KHÔNG có buffer pool — đọc/ghi trực tiếp qua PageManager.
 * Buffer pool sẽ thêm ở Phase 3. Hiện tại để khẳng định B-tree disk
 * đúng 100% trước khi optimize.
 */
#ifndef HUGO_DBTREE_H
#define HUGO_DBTREE_H

#include <stdint.h>
#include "page.h"
#include "btree.h"   /* Reuse BTREE_T, BTREE_MAX_KEYS, key_t, val_t */

/* Disk node header offsets trong page->data */
#define DN_ISLEAF_OFF   0
#define DN_NUMKEYS_OFF  1
#define DN_PAD_OFF      3
#define DN_KEYS_OFF     5

typedef struct {
    PageManager *pm;
} DBTree;

/* Result codes — reuse BT_* */

int dbt_init   (DBTree *t, PageManager *pm);   /* attach to existing pm */
int dbt_close  (DBTree *t);

int dbt_insert (DBTree *t, btree_key_t k, btree_val_t v);
int dbt_search (DBTree *t, btree_key_t k, btree_val_t *out);
int dbt_delete (DBTree *t, btree_key_t k);

/* Debug: in cây ra stdout */
void dbt_print (DBTree *t);

/* Verify invariants — đọc toàn cây từ disk */
int  dbt_check_invariants(DBTree *t);

#endif
