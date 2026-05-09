/* btree.c — In-memory B-tree
 *
 * Thuật toán theo CLRS chương 18.
 * Insert: split-on-the-way-down (preemptive split khi gặp full child).
 * Delete: ensure-min-keys-on-the-way-down (preemptive merge/borrow).
 *
 * Hai chiến lược preemptive đảm bảo recursion 1 chiều, không bao giờ
 * cần đi ngược lên — đơn giản và đúng đắn.
 */
#include "btree.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ========== Node alloc ========== */
static BTreeNode* node_new(int is_leaf) {
    BTreeNode *n = (BTreeNode*)calloc(1, sizeof(BTreeNode));
    if (!n) return NULL;
    n->is_leaf = is_leaf;
    n->num_keys = 0;
    return n;
}

static void node_free(BTreeNode *n) {
    if (!n) return;
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; i++) node_free(n->children[i]);
    }
    free(n);
}

/* ========== Lifecycle ========== */
BTree* btree_create(void) {
    BTree *t = (BTree*)calloc(1, sizeof(BTree));
    if (!t) return NULL;
    t->root = node_new(1);  /* root khởi đầu là leaf rỗng */
    t->count = 0;
    return t;
}

void btree_destroy(BTree *t) {
    if (!t) return;
    node_free(t->root);
    free(t);
}

/* ========== Search ========== */
static int node_find_index(const BTreeNode *n, btree_key_t k) {
    /* Trả về i đầu tiên sao cho keys[i] >= k. Linear scan đủ với t nhỏ. */
    int i = 0;
    while (i < n->num_keys && n->keys[i] < k) i++;
    return i;
}

static int node_search(const BTreeNode *n, btree_key_t k, btree_val_t *out) {
    int i = node_find_index(n, k);
    if (i < n->num_keys && n->keys[i] == k) {
        /* Found — internal node CŨNG lưu value (vì khi split, median
         * đẩy lên kèm value của nó). */
        if (out) *out = n->values[i];
        return BT_OK;
    }
    if (n->is_leaf) return BT_NOT_FOUND;
    return node_search(n->children[i], k, out);
}

int btree_search(const BTree *t, btree_key_t k, btree_val_t *out) {
    if (!t || !t->root) return BT_NOT_FOUND;
    if (t->root->num_keys == 0) return BT_NOT_FOUND;
    return node_search(t->root, k, out);
}

/* ========== Insert (preemptive split) ========== */

/* split_child: y = x->children[i] đang full (2t-1 keys). Tách thành 2 node,
 * đẩy median lên x ở vị trí i. */
static void split_child(BTreeNode *x, int i) {
    BTreeNode *y = x->children[i];
    BTreeNode *z = node_new(y->is_leaf);
    z->num_keys = BTREE_T - 1;

    /* Copy nửa sau của y sang z */
    for (int j = 0; j < BTREE_T - 1; j++) {
        z->keys[j]   = y->keys[j + BTREE_T];
        z->values[j] = y->values[j + BTREE_T];
    }
    if (!y->is_leaf) {
        for (int j = 0; j < BTREE_T; j++) {
            z->children[j] = y->children[j + BTREE_T];
            y->children[j + BTREE_T] = NULL;
        }
    }
    y->num_keys = BTREE_T - 1;

    /* Shift children của x sang phải để chèn z */
    for (int j = x->num_keys; j >= i + 1; j--) {
        x->children[j + 1] = x->children[j];
    }
    x->children[i + 1] = z;

    /* Shift keys của x */
    for (int j = x->num_keys - 1; j >= i; j--) {
        x->keys[j + 1]   = x->keys[j];
        x->values[j + 1] = x->values[j];
    }
    /* Đẩy median của y lên x */
    x->keys[i]   = y->keys[BTREE_T - 1];
    x->values[i] = y->values[BTREE_T - 1];
    x->num_keys++;
}

/* Insert vào node chưa full. Trả về BT_OK hoặc BT_DUP. */
static int insert_nonfull(BTreeNode *x, btree_key_t k, btree_val_t v) {
    int i = x->num_keys - 1;

    if (x->is_leaf) {
        /* Check duplicate */
        int idx = node_find_index(x, k);
        if (idx < x->num_keys && x->keys[idx] == k) return BT_DUP;

        while (i >= 0 && x->keys[i] > k) {
            x->keys[i + 1]   = x->keys[i];
            x->values[i + 1] = x->values[i];
            i--;
        }
        x->keys[i + 1]   = k;
        x->values[i + 1] = v;
        x->num_keys++;
        return BT_OK;
    }

    /* Internal: tìm child đi xuống */
    while (i >= 0 && x->keys[i] > k) i--;
    /* Check duplicate ở separator */
    if (i >= 0 && x->keys[i] == k) return BT_DUP;
    i++;

    if (x->children[i]->num_keys == BTREE_MAX_KEYS) {
        split_child(x, i);
        if (x->keys[i] == k) return BT_DUP;
        if (x->keys[i] < k) i++;
    }
    return insert_nonfull(x->children[i], k, v);
}

int btree_insert(BTree *t, btree_key_t k, btree_val_t v) {
    if (!t) return BT_INVARIANT_BAD;
    BTreeNode *r = t->root;
    if (r->num_keys == BTREE_MAX_KEYS) {
        /* Split root: tạo root mới */
        BTreeNode *s = node_new(0);
        s->children[0] = r;
        split_child(s, 0);
        t->root = s;
        int rc = insert_nonfull(s, k, v);
        if (rc == BT_OK) t->count++;
        return rc;
    }
    int rc = insert_nonfull(r, k, v);
    if (rc == BT_OK) t->count++;
    return rc;
}

/* ========== Delete (preemptive merge/borrow) ==========
 * Quy tắc trước khi descend vào child[i]: đảm bảo child[i] có >= t keys.
 * Nếu không, borrow từ sibling hoặc merge.
 */

static btree_key_t pred_key(BTreeNode *x, int i, btree_val_t *out_val) {
    /* Predecessor của keys[i]: key lớn nhất trong subtree children[i] */
    BTreeNode *cur = x->children[i];
    while (!cur->is_leaf) cur = cur->children[cur->num_keys];
    if (out_val) *out_val = cur->values[cur->num_keys - 1];
    return cur->keys[cur->num_keys - 1];
}

static btree_key_t succ_key(BTreeNode *x, int i, btree_val_t *out_val) {
    /* Successor của keys[i]: key nhỏ nhất trong subtree children[i+1] */
    BTreeNode *cur = x->children[i + 1];
    while (!cur->is_leaf) cur = cur->children[0];
    if (out_val) *out_val = cur->values[0];
    return cur->keys[0];
}

/* Merge child[i] và child[i+1] dùng keys[i] làm separator (chỉ với internal) */
static void merge_children(BTreeNode *x, int i) {
    BTreeNode *y = x->children[i];
    BTreeNode *z = x->children[i + 1];

    /* y bây giờ có t-1 keys. Thêm separator + z vào y → 2t-1 keys */
    y->keys[BTREE_T - 1]   = x->keys[i];
    y->values[BTREE_T - 1] = x->values[i];

    for (int j = 0; j < z->num_keys; j++) {
        y->keys[BTREE_T + j]   = z->keys[j];
        y->values[BTREE_T + j] = z->values[j];
    }
    if (!y->is_leaf) {
        for (int j = 0; j <= z->num_keys; j++) {
            y->children[BTREE_T + j] = z->children[j];
            z->children[j] = NULL;
        }
    }
    y->num_keys = 2 * BTREE_T - 1;

    /* Bỏ separator khỏi x, shift children */
    for (int j = i; j < x->num_keys - 1; j++) {
        x->keys[j]   = x->keys[j + 1];
        x->values[j] = x->values[j + 1];
    }
    for (int j = i + 1; j < x->num_keys; j++) {
        x->children[j] = x->children[j + 1];
    }
    x->children[x->num_keys] = NULL;
    x->num_keys--;

    free(z);
}

/* Đảm bảo x->children[i] có >= t keys. Trả về index đã điều chỉnh
 * (sau merge có thể i đổi vì child[i] có thể đã merge với child[i-1]). */
static int ensure_min_keys(BTreeNode *x, int i) {
    BTreeNode *c = x->children[i];
    if (c->num_keys >= BTREE_T) return i;

    BTreeNode *left  = (i > 0)              ? x->children[i - 1] : NULL;
    BTreeNode *right = (i < x->num_keys)    ? x->children[i + 1] : NULL;

    /* Borrow từ left sibling */
    if (left && left->num_keys >= BTREE_T) {
        /* Shift c phải 1 chỗ */
        for (int j = c->num_keys; j > 0; j--) {
            c->keys[j]   = c->keys[j - 1];
            c->values[j] = c->values[j - 1];
        }
        if (!c->is_leaf) {
            for (int j = c->num_keys + 1; j > 0; j--)
                c->children[j] = c->children[j - 1];
        }
        /* Đưa separator x->keys[i-1] xuống đầu c */
        c->keys[0]   = x->keys[i - 1];
        c->values[0] = x->values[i - 1];
        if (!c->is_leaf) {
            c->children[0] = left->children[left->num_keys];
            left->children[left->num_keys] = NULL;
        }
        /* Đưa key cuối left lên separator */
        x->keys[i - 1]   = left->keys[left->num_keys - 1];
        x->values[i - 1] = left->values[left->num_keys - 1];
        c->num_keys++;
        left->num_keys--;
        return i;
    }
    /* Borrow từ right sibling */
    if (right && right->num_keys >= BTREE_T) {
        c->keys[c->num_keys]   = x->keys[i];
        c->values[c->num_keys] = x->values[i];
        if (!c->is_leaf) {
            c->children[c->num_keys + 1] = right->children[0];
        }
        x->keys[i]   = right->keys[0];
        x->values[i] = right->values[0];
        for (int j = 0; j < right->num_keys - 1; j++) {
            right->keys[j]   = right->keys[j + 1];
            right->values[j] = right->values[j + 1];
        }
        if (!right->is_leaf) {
            for (int j = 0; j < right->num_keys; j++)
                right->children[j] = right->children[j + 1];
            right->children[right->num_keys] = NULL;
        }
        c->num_keys++;
        right->num_keys--;
        return i;
    }
    /* Không borrow được → merge */
    if (right) {
        merge_children(x, i);
        return i;
    } else {
        merge_children(x, i - 1);
        return i - 1;
    }
}

static int delete_from(BTreeNode *x, btree_key_t k);

/* Xoá key tại vị trí i trong leaf x */
static void delete_leaf_at(BTreeNode *x, int i) {
    for (int j = i; j < x->num_keys - 1; j++) {
        x->keys[j]   = x->keys[j + 1];
        x->values[j] = x->values[j + 1];
    }
    x->num_keys--;
}

/* Xoá key k tại vị trí i trong internal node x */
static int delete_internal_at(BTreeNode *x, int i) {
    btree_key_t k = x->keys[i];

    /* Case 2a: child[i] có >= t keys → thay bằng predecessor, xoá pred khỏi subtree */
    if (x->children[i]->num_keys >= BTREE_T) {
        btree_val_t pv;
        btree_key_t pk = pred_key(x, i, &pv);
        x->keys[i]   = pk;
        x->values[i] = pv;
        return delete_from(x->children[i], pk);
    }
    /* Case 2b: child[i+1] có >= t keys → thay bằng successor */
    if (x->children[i + 1]->num_keys >= BTREE_T) {
        btree_val_t sv;
        btree_key_t sk = succ_key(x, i, &sv);
        x->keys[i]   = sk;
        x->values[i] = sv;
        return delete_from(x->children[i + 1], sk);
    }
    /* Case 2c: cả 2 đều có t-1 keys → merge */
    merge_children(x, i);
    return delete_from(x->children[i], k);
}

static int delete_from(BTreeNode *x, btree_key_t k) {
    int i = node_find_index(x, k);

    if (i < x->num_keys && x->keys[i] == k) {
        if (x->is_leaf) {
            delete_leaf_at(x, i);
            return BT_OK;
        }
        return delete_internal_at(x, i);
    }
    if (x->is_leaf) return BT_NOT_FOUND;

    /* Đảm bảo child mình sắp đi xuống có >= t keys */
    int adj_i = ensure_min_keys(x, i);
    /* Sau ensure_min_keys, key có thể đã chuyển vị trí. Tìm lại. */
    return delete_from(x->children[adj_i], k);
}

int btree_delete(BTree *t, btree_key_t k) {
    if (!t || !t->root) return BT_NOT_FOUND;
    if (t->root->num_keys == 0) return BT_NOT_FOUND;

    int rc = delete_from(t->root, k);

    /* Nếu root rỗng và là internal, hạ root xuống child duy nhất */
    if (t->root->num_keys == 0 && !t->root->is_leaf) {
        BTreeNode *old = t->root;
        t->root = old->children[0];
        old->children[0] = NULL;
        free(old);
    }

    if (rc == BT_OK) t->count--;
    return rc;
}

/* ========== Invariants ==========
 * Verify:
 *   1. Keys sorted strictly ascending tại mọi node
 *   2. Mọi non-root node có [t-1 .. 2t-1] keys
 *   3. Internal node có (num_keys + 1) children, không NULL
 *   4. Mọi leaf cùng depth
 *   5. Children keys nằm đúng range của separator
 *
 * Trả về 0 nếu OK, !=0 nếu có vấn đề.
 */

static int check_node(const BTreeNode *n, int is_root, int depth,
                      int *leaf_depth,
                      btree_key_t lo, int has_lo,
                      btree_key_t hi, int has_hi) {
    if (!n) return 1;

    /* (2) num_keys range */
    if (!is_root) {
        if (n->num_keys < BTREE_MIN_KEYS) return 2;
        if (n->num_keys > BTREE_MAX_KEYS) return 3;
    } else {
        if (n->num_keys > BTREE_MAX_KEYS) return 3;
    }

    /* (1) keys sorted strictly */
    for (int i = 1; i < n->num_keys; i++) {
        if (n->keys[i - 1] >= n->keys[i]) return 4;
    }
    /* (5) keys in [lo, hi] */
    for (int i = 0; i < n->num_keys; i++) {
        if (has_lo && n->keys[i] <= lo) return 5;
        if (has_hi && n->keys[i] >= hi) return 6;
    }

    if (n->is_leaf) {
        /* (4) leaf depth must match */
        if (*leaf_depth == -1) *leaf_depth = depth;
        else if (*leaf_depth != depth) return 7;
        return 0;
    }

    /* (3) internal có num_keys+1 children */
    for (int i = 0; i <= n->num_keys; i++) {
        if (!n->children[i]) return 8;
    }

    /* Recurse từng child với range thu hẹp */
    for (int i = 0; i <= n->num_keys; i++) {
        btree_key_t child_lo = lo;
        btree_key_t child_hi = hi;
        int child_has_lo = has_lo;
        int child_has_hi = has_hi;

        if (i > 0) {
            child_lo = n->keys[i - 1];
            child_has_lo = 1;
        }
        if (i < n->num_keys) {
            child_hi = n->keys[i];
            child_has_hi = 1;
        }
        int rc = check_node(n->children[i], 0, depth + 1, leaf_depth,
                            child_lo, child_has_lo,
                            child_hi, child_has_hi);
        if (rc != 0) return rc;
    }
    return 0;
}

int btree_check_invariants(const BTree *t) {
    if (!t || !t->root) return 100;
    int leaf_depth = -1;
    return check_node(t->root, 1, 0, &leaf_depth, 0, 0, 0, 0);
}

/* ========== ASCII visualizer ========== */
static void print_node(const BTreeNode *n, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    printf("[");
    for (int i = 0; i < n->num_keys; i++) {
        printf("%llu", (unsigned long long)n->keys[i]);
        if (i + 1 < n->num_keys) printf(",");
    }
    printf("]%s\n", n->is_leaf ? " (leaf)" : "");
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; i++) {
            print_node(n->children[i], depth + 1);
        }
    }
}

void btree_print(const BTree *t) {
    if (!t || !t->root) { printf("(empty tree)\n"); return; }
    print_node(t->root, 0);
}
