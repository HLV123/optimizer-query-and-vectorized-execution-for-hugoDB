/* btree.h — In-memory B-tree (Phase 2, bước 1)
 *
 * Order: minimum degree t = BTREE_T.
 *   - Mỗi node (trừ root) có [t-1 .. 2t-1] keys
 *   - Internal node có (num_keys + 1) children
 *   - Root được phép có 1..2t-1 keys; nếu rỗng → cây rỗng
 *   - Mọi leaf cùng depth
 *
 * Key   = uint64_t (binary sortable)
 * Value = uint64_t (sau này map sang page_id của document)
 *
 * API:
 *   btree_create / btree_destroy
 *   btree_insert(tree, key, value)
 *   btree_search(tree, key, *out_value) → 0 nếu found, -1 nếu không
 *   btree_delete(tree, key)             → 0 nếu xoá được, -1 nếu không có
 *   btree_check_invariants(tree)        → 0 nếu OK, !=0 nếu vi phạm
 *   btree_print(tree)                   → ASCII visualizer (debug)
 */
#ifndef HUGO_BTREE_H
#define HUGO_BTREE_H

#include <stdint.h>
#include <stddef.h>

#ifndef BTREE_T
#define BTREE_T 4   /* min degree — dễ test với t nhỏ; production có thể tăng */
#endif

#define BTREE_MAX_KEYS     (2 * BTREE_T - 1)
#define BTREE_MIN_KEYS     (BTREE_T - 1)
#define BTREE_MAX_CHILDREN (2 * BTREE_T)

typedef uint64_t btree_key_t;
typedef uint64_t btree_val_t;

typedef struct BTreeNode {
    int               num_keys;
    int               is_leaf;
    btree_key_t       keys[BTREE_MAX_KEYS];
    btree_val_t       values[BTREE_MAX_KEYS];          /* dùng cho leaf */
    struct BTreeNode *children[BTREE_MAX_CHILDREN];    /* dùng cho internal */
} BTreeNode;

typedef struct {
    BTreeNode *root;
    size_t     count;   /* tổng số key trong cây — dùng cho test */
} BTree;

/* Result codes */
#define BT_OK             0
#define BT_NOT_FOUND     -1
#define BT_DUP           -2  /* key đã tồn tại — chính sách: reject duplicate */
#define BT_INVARIANT_BAD -3

BTree* btree_create(void);
void   btree_destroy(BTree *t);

int    btree_insert(BTree *t, btree_key_t k, btree_val_t v);
int    btree_search(const BTree *t, btree_key_t k, btree_val_t *out);
int    btree_delete(BTree *t, btree_key_t k);

/* Invariant checker — dùng nặng trong test */
int    btree_check_invariants(const BTree *t);

/* ASCII visualizer */
void   btree_print(const BTree *t);

#endif
