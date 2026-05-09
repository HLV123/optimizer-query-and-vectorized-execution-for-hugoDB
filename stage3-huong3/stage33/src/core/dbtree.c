/* dbtree.c — Disk-backed B-tree
 *
 * Đối xứng 1-1 với btree.c (RAM version) nhưng:
 *   - "Node" được load lên qua disk_load(page_id, &node)
 *   - Sau khi sửa node → disk_store(node)
 *   - Children là page_id, không phải pointer
 *
 * Để giữ code rõ ràng, dùng struct DiskNode trên stack — load/store
 * mỗi lần. KHÔNG cache — Phase 3 sẽ thêm buffer pool.
 */
#include "dbtree.h"
#include "serializer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* In-memory representation của 1 disk node (đã deserialize) */
typedef struct {
    uint64_t      page_id;       /* page_id chứa node này */
    int           is_leaf;
    int           num_keys;
    btree_key_t   keys[BTREE_MAX_KEYS];
    btree_val_t   values[BTREE_MAX_KEYS];
    uint64_t      children[BTREE_MAX_CHILDREN];   /* page_id của children */
} DNode;

/* ===== Serialize / Deserialize node ===== */
static void dnode_serialize(const DNode *n, uint8_t *data) {
    /* data trỏ vào page->data (HUGO_PAGE_DATA_SIZE bytes) */
    memset(data, 0, HUGO_PAGE_DATA_SIZE);
    data[DN_ISLEAF_OFF] = (uint8_t)(n->is_leaf ? 1 : 0);
    write_u16_be(data + DN_NUMKEYS_OFF, (uint16_t)n->num_keys);

    uint8_t *p = data + DN_KEYS_OFF;
    for (int i = 0; i < n->num_keys; i++) { write_u64_be(p, n->keys[i]); p += 8; }
    for (int i = 0; i < n->num_keys; i++) { write_u64_be(p, n->values[i]); p += 8; }
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; i++) {
            write_u64_be(p, n->children[i]); p += 8;
        }
    }
}

static void dnode_deserialize(DNode *n, const uint8_t *data, uint64_t page_id) {
    n->page_id  = page_id;
    n->is_leaf  = data[DN_ISLEAF_OFF] ? 1 : 0;
    n->num_keys = read_u16_be(data + DN_NUMKEYS_OFF);

    const uint8_t *p = data + DN_KEYS_OFF;
    for (int i = 0; i < n->num_keys; i++) { n->keys[i]   = read_u64_be(p); p += 8; }
    for (int i = 0; i < n->num_keys; i++) { n->values[i] = read_u64_be(p); p += 8; }
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; i++) {
            n->children[i] = read_u64_be(p); p += 8;
        }
    } else {
        for (int i = 0; i < BTREE_MAX_CHILDREN; i++) n->children[i] = 0;
    }
}

/* ===== Load / Store qua PageManager ===== */
static int dnode_load(PageManager *pm, uint64_t page_id, DNode *out) {
    HugoPage page;
    int rc = pm_read_page(pm, page_id, &page);
    if (rc != PG_OK) return BT_INVARIANT_BAD;
    if (page.page_type != PAGE_TYPE_LEAF && page.page_type != PAGE_TYPE_INTERNAL)
        return BT_INVARIANT_BAD;
    dnode_deserialize(out, page.data, page_id);
    return BT_OK;
}

static int dnode_store(PageManager *pm, const DNode *n) {
    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id  = (uint32_t)n->page_id;
    page.page_type = (uint8_t)(n->is_leaf ? PAGE_TYPE_LEAF : PAGE_TYPE_INTERNAL);
    page.num_keys = (uint16_t)n->num_keys;
    dnode_serialize(n, page.data);
    int rc = pm_write_page(pm, &page);
    if (rc != PG_OK) return BT_INVARIANT_BAD;
    return BT_OK;
}

/* Tạo node mới: alloc page, init, store */
static int dnode_new(PageManager *pm, int is_leaf, DNode *out) {
    uint64_t pid;
    if (pm_alloc_page(pm, &pid) != PG_OK) return BT_INVARIANT_BAD;
    out->page_id  = pid;
    out->is_leaf  = is_leaf;
    out->num_keys = 0;
    for (int i = 0; i < BTREE_MAX_CHILDREN; i++) out->children[i] = 0;
    return dnode_store(pm, out);
}

/* Free node → mark page là FREE (chưa có free list reuse, nhưng đỡ orphan) */
static void dnode_free(PageManager *pm, uint64_t page_id) {
    /* MVP: không reuse page, chỉ skip. Free list sẽ làm sau. */
    (void)pm; (void)page_id;
}

/* ===== Lifecycle ===== */
int dbt_init(DBTree *t, PageManager *pm) {
    t->pm = pm;
    return BT_OK;
}

int dbt_close(DBTree *t) {
    /* Flush header (root_page có thể đã thay đổi) */
    return pm_flush_header(t->pm) == PG_OK ? BT_OK : BT_INVARIANT_BAD;
}

/* ===== Search ===== */
static int dnode_find_index(const DNode *n, btree_key_t k) {
    int i = 0;
    while (i < n->num_keys && n->keys[i] < k) i++;
    return i;
}

static int dbt_search_node(PageManager *pm, uint64_t page_id,
                            btree_key_t k, btree_val_t *out) {
    DNode n;
    int rc = dnode_load(pm, page_id, &n);
    if (rc != BT_OK) return rc;

    int i = dnode_find_index(&n, k);
    if (i < n.num_keys && n.keys[i] == k) {
        if (out) *out = n.values[i];
        return BT_OK;
    }
    if (n.is_leaf) return BT_NOT_FOUND;
    return dbt_search_node(pm, n.children[i], k, out);
}

int dbt_search(DBTree *t, btree_key_t k, btree_val_t *out) {
    if (t->pm->hdr.root_page == 0) return BT_NOT_FOUND;
    return dbt_search_node(t->pm, t->pm->hdr.root_page, k, out);
}

/* ===== Insert ===== */

/* split_child: x (loaded) đang chứa pointer tới y = x->children[i] full.
 * Tạo z, copy nửa sau y sang z, đẩy median lên x. STORE cả 3 node. */
static int split_child(PageManager *pm, DNode *x, int i) {
    DNode y;
    int rc = dnode_load(pm, x->children[i], &y);
    if (rc != BT_OK) return rc;

    DNode z;
    rc = dnode_new(pm, y.is_leaf, &z);
    if (rc != BT_OK) return rc;

    z.num_keys = BTREE_T - 1;
    for (int j = 0; j < BTREE_T - 1; j++) {
        z.keys[j]   = y.keys[j + BTREE_T];
        z.values[j] = y.values[j + BTREE_T];
    }
    if (!y.is_leaf) {
        for (int j = 0; j < BTREE_T; j++) {
            z.children[j] = y.children[j + BTREE_T];
            y.children[j + BTREE_T] = 0;
        }
    }
    y.num_keys = BTREE_T - 1;

    /* Shift x->children right */
    for (int j = x->num_keys; j >= i + 1; j--) {
        x->children[j + 1] = x->children[j];
    }
    x->children[i + 1] = z.page_id;

    /* Shift x->keys right */
    for (int j = x->num_keys - 1; j >= i; j--) {
        x->keys[j + 1]   = x->keys[j];
        x->values[j + 1] = x->values[j];
    }
    x->keys[i]   = y.keys[BTREE_T - 1];
    x->values[i] = y.values[BTREE_T - 1];
    x->num_keys++;

    /* Store y, z, x */
    if ((rc = dnode_store(pm, &y)) != BT_OK) return rc;
    if ((rc = dnode_store(pm, &z)) != BT_OK) return rc;
    if ((rc = dnode_store(pm, x)) != BT_OK) return rc;
    return BT_OK;
}

static int insert_nonfull(PageManager *pm, DNode *x, btree_key_t k, btree_val_t v) {
    int i = x->num_keys - 1;

    if (x->is_leaf) {
        int idx = dnode_find_index(x, k);
        if (idx < x->num_keys && x->keys[idx] == k) return BT_DUP;

        while (i >= 0 && x->keys[i] > k) {
            x->keys[i + 1]   = x->keys[i];
            x->values[i + 1] = x->values[i];
            i--;
        }
        x->keys[i + 1]   = k;
        x->values[i + 1] = v;
        x->num_keys++;
        return dnode_store(pm, x);
    }

    while (i >= 0 && x->keys[i] > k) i--;
    if (i >= 0 && x->keys[i] == k) return BT_DUP;
    i++;

    /* Load child[i] để check fullness */
    DNode child;
    int rc = dnode_load(pm, x->children[i], &child);
    if (rc != BT_OK) return rc;

    if (child.num_keys == BTREE_MAX_KEYS) {
        rc = split_child(pm, x, i);
        if (rc != BT_OK) return rc;
        if (x->keys[i] == k) return BT_DUP;
        if (x->keys[i] < k) i++;
        rc = dnode_load(pm, x->children[i], &child);
        if (rc != BT_OK) return rc;
    }
    return insert_nonfull(pm, &child, k, v);
}

int dbt_insert(DBTree *t, btree_key_t k, btree_val_t v) {
    PageManager *pm = t->pm;

    if (pm->hdr.root_page == 0) {
        DNode root;
        int rc = dnode_new(pm, 1, &root);
        if (rc != BT_OK) return rc;
        root.num_keys = 1;
        root.keys[0] = k;
        root.values[0] = v;
        rc = dnode_store(pm, &root);
        if (rc != BT_OK) return rc;
        pm->hdr.root_page = root.page_id;
        pm_flush_header(pm);
        return BT_OK;
    }

    DNode root;
    int rc = dnode_load(pm, pm->hdr.root_page, &root);
    if (rc != BT_OK) return rc;

    if (root.num_keys == BTREE_MAX_KEYS) {
        DNode new_root;
        rc = dnode_new(pm, 0, &new_root);
        if (rc != BT_OK) return rc;
        new_root.children[0] = root.page_id;
        rc = dnode_store(pm, &new_root);
        if (rc != BT_OK) return rc;

        rc = split_child(pm, &new_root, 0);
        if (rc != BT_OK) return rc;

        pm->hdr.root_page = new_root.page_id;
        pm_flush_header(pm);

        rc = insert_nonfull(pm, &new_root, k, v);
        return rc;
    }
    return insert_nonfull(pm, &root, k, v);
}

/* ===== Delete ===== */

static btree_key_t pred_descent(PageManager *pm, uint64_t child_pid,
                                 btree_val_t *out_v) {
    DNode cur;
    dnode_load(pm, child_pid, &cur);
    while (!cur.is_leaf) {
        dnode_load(pm, cur.children[cur.num_keys], &cur);
    }
    if (out_v) *out_v = cur.values[cur.num_keys - 1];
    return cur.keys[cur.num_keys - 1];
}

static btree_key_t succ_descent(PageManager *pm, uint64_t child_pid,
                                 btree_val_t *out_v) {
    DNode cur;
    dnode_load(pm, child_pid, &cur);
    while (!cur.is_leaf) {
        dnode_load(pm, cur.children[0], &cur);
    }
    if (out_v) *out_v = cur.values[0];
    return cur.keys[0];
}

/* Merge x->children[i] và x->children[i+1] dùng x->keys[i] làm separator.
 * Sau merge: child[i+1] page bị orphan (free list sẽ xử lý sau).
 * Returns: BT_OK on success */
static int merge_children(PageManager *pm, DNode *x, int i) {
    DNode y, z;
    int rc;
    if ((rc = dnode_load(pm, x->children[i],     &y)) != BT_OK) return rc;
    if ((rc = dnode_load(pm, x->children[i + 1], &z)) != BT_OK) return rc;

    y.keys[BTREE_T - 1]   = x->keys[i];
    y.values[BTREE_T - 1] = x->values[i];

    for (int j = 0; j < z.num_keys; j++) {
        y.keys[BTREE_T + j]   = z.keys[j];
        y.values[BTREE_T + j] = z.values[j];
    }
    if (!y.is_leaf) {
        for (int j = 0; j <= z.num_keys; j++) {
            y.children[BTREE_T + j] = z.children[j];
        }
    }
    y.num_keys = 2 * BTREE_T - 1;

    /* Bỏ separator + child[i+1] khỏi x */
    for (int j = i; j < x->num_keys - 1; j++) {
        x->keys[j]   = x->keys[j + 1];
        x->values[j] = x->values[j + 1];
    }
    for (int j = i + 1; j < x->num_keys; j++) {
        x->children[j] = x->children[j + 1];
    }
    x->children[x->num_keys] = 0;
    x->num_keys--;

    dnode_free(pm, z.page_id);

    if ((rc = dnode_store(pm, &y)) != BT_OK) return rc;
    if ((rc = dnode_store(pm, x)) != BT_OK) return rc;
    return BT_OK;
}

/* Borrow/merge để đảm bảo x->children[i] có >= t keys.
 * Trả về: index đã điều chỉnh (qua *out_i), BT_OK/error qua return. */
static int ensure_min_keys(PageManager *pm, DNode *x, int i, int *out_i) {
    DNode c;
    int rc = dnode_load(pm, x->children[i], &c);
    if (rc != BT_OK) return rc;
    if (c.num_keys >= BTREE_T) { *out_i = i; return BT_OK; }

    DNode left, right;
    int has_left  = (i > 0);
    int has_right = (i < x->num_keys);
    if (has_left)  dnode_load(pm, x->children[i - 1], &left);
    if (has_right) dnode_load(pm, x->children[i + 1], &right);

    /* Borrow từ left */
    if (has_left && left.num_keys >= BTREE_T) {
        for (int j = c.num_keys; j > 0; j--) {
            c.keys[j]   = c.keys[j - 1];
            c.values[j] = c.values[j - 1];
        }
        if (!c.is_leaf) {
            for (int j = c.num_keys + 1; j > 0; j--)
                c.children[j] = c.children[j - 1];
        }
        c.keys[0]   = x->keys[i - 1];
        c.values[0] = x->values[i - 1];
        if (!c.is_leaf) {
            c.children[0] = left.children[left.num_keys];
            left.children[left.num_keys] = 0;
        }
        x->keys[i - 1]   = left.keys[left.num_keys - 1];
        x->values[i - 1] = left.values[left.num_keys - 1];
        c.num_keys++;
        left.num_keys--;
        dnode_store(pm, &left);
        dnode_store(pm, &c);
        dnode_store(pm, x);
        *out_i = i;
        return BT_OK;
    }
    /* Borrow từ right */
    if (has_right && right.num_keys >= BTREE_T) {
        c.keys[c.num_keys]   = x->keys[i];
        c.values[c.num_keys] = x->values[i];
        if (!c.is_leaf) c.children[c.num_keys + 1] = right.children[0];
        x->keys[i]   = right.keys[0];
        x->values[i] = right.values[0];
        for (int j = 0; j < right.num_keys - 1; j++) {
            right.keys[j]   = right.keys[j + 1];
            right.values[j] = right.values[j + 1];
        }
        if (!right.is_leaf) {
            for (int j = 0; j < right.num_keys; j++)
                right.children[j] = right.children[j + 1];
            right.children[right.num_keys] = 0;
        }
        c.num_keys++;
        right.num_keys--;
        dnode_store(pm, &right);
        dnode_store(pm, &c);
        dnode_store(pm, x);
        *out_i = i;
        return BT_OK;
    }
    /* Merge */
    if (has_right) {
        rc = merge_children(pm, x, i);
        *out_i = i;
        return rc;
    } else {
        rc = merge_children(pm, x, i - 1);
        *out_i = i - 1;
        return rc;
    }
}

static int delete_from(PageManager *pm, uint64_t node_pid, btree_key_t k);

static int delete_internal_at(PageManager *pm, DNode *x, int i) {
    btree_key_t k = x->keys[i];

    DNode left_c, right_c;
    int rc;
    if ((rc = dnode_load(pm, x->children[i],     &left_c))  != BT_OK) return rc;
    if ((rc = dnode_load(pm, x->children[i + 1], &right_c)) != BT_OK) return rc;

    if (left_c.num_keys >= BTREE_T) {
        btree_val_t pv;
        btree_key_t pk = pred_descent(pm, x->children[i], &pv);
        x->keys[i]   = pk;
        x->values[i] = pv;
        dnode_store(pm, x);
        return delete_from(pm, x->children[i], pk);
    }
    if (right_c.num_keys >= BTREE_T) {
        btree_val_t sv;
        btree_key_t sk = succ_descent(pm, x->children[i + 1], &sv);
        x->keys[i]   = sk;
        x->values[i] = sv;
        dnode_store(pm, x);
        return delete_from(pm, x->children[i + 1], sk);
    }
    /* Merge children[i] và children[i+1] */
    uint64_t merged_pid = x->children[i];
    rc = merge_children(pm, x, i);
    if (rc != BT_OK) return rc;
    return delete_from(pm, merged_pid, k);
}

static int delete_from(PageManager *pm, uint64_t node_pid, btree_key_t k) {
    DNode x;
    int rc = dnode_load(pm, node_pid, &x);
    if (rc != BT_OK) return rc;

    int i = dnode_find_index(&x, k);

    if (i < x.num_keys && x.keys[i] == k) {
        if (x.is_leaf) {
            for (int j = i; j < x.num_keys - 1; j++) {
                x.keys[j]   = x.keys[j + 1];
                x.values[j] = x.values[j + 1];
            }
            x.num_keys--;
            return dnode_store(pm, &x);
        }
        return delete_internal_at(pm, &x, i);
    }
    if (x.is_leaf) return BT_NOT_FOUND;

    int adj_i;
    rc = ensure_min_keys(pm, &x, i, &adj_i);
    if (rc != BT_OK) return rc;

    /* Reload x từ disk vì merge_children/borrow có thể đã store version mới */
    rc = dnode_load(pm, node_pid, &x);
    if (rc != BT_OK) return rc;
    return delete_from(pm, x.children[adj_i], k);
}

int dbt_delete(DBTree *t, btree_key_t k) {
    PageManager *pm = t->pm;
    if (pm->hdr.root_page == 0) return BT_NOT_FOUND;

    int rc = delete_from(pm, pm->hdr.root_page, k);
    if (rc != BT_OK) return rc;

    /* Nếu root rỗng và là internal → hạ root */
    DNode root;
    if (dnode_load(pm, pm->hdr.root_page, &root) != BT_OK) return rc;
    if (root.num_keys == 0) {
        if (!root.is_leaf) {
            uint64_t old = pm->hdr.root_page;
            pm->hdr.root_page = root.children[0];
            dnode_free(pm, old);
            pm_flush_header(pm);
        } else {
            /* Leaf rỗng → cây rỗng */
            uint64_t old = pm->hdr.root_page;
            pm->hdr.root_page = 0;
            dnode_free(pm, old);
            pm_flush_header(pm);
        }
    }
    return BT_OK;
}

/* ===== Visualizer + invariants ===== */
static void print_node_d(PageManager *pm, uint64_t pid, int depth) {
    DNode n;
    if (dnode_load(pm, pid, &n) != BT_OK) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("(load failed pid=%llu)\n", (unsigned long long)pid);
        return;
    }
    for (int i = 0; i < depth; i++) printf("  ");
    printf("[");
    for (int i = 0; i < n.num_keys; i++) {
        printf("%llu", (unsigned long long)n.keys[i]);
        if (i + 1 < n.num_keys) printf(",");
    }
    printf("] pid=%llu%s\n", (unsigned long long)pid, n.is_leaf ? " (leaf)" : "");
    if (!n.is_leaf) {
        for (int i = 0; i <= n.num_keys; i++) {
            print_node_d(pm, n.children[i], depth + 1);
        }
    }
}

void dbt_print(DBTree *t) {
    if (t->pm->hdr.root_page == 0) { printf("(empty disk tree)\n"); return; }
    print_node_d(t->pm, t->pm->hdr.root_page, 0);
}

static int check_d(PageManager *pm, uint64_t pid, int is_root, int depth,
                   int *leaf_depth,
                   btree_key_t lo, int has_lo,
                   btree_key_t hi, int has_hi) {
    DNode n;
    if (dnode_load(pm, pid, &n) != BT_OK) return 1;

    if (!is_root) {
        if (n.num_keys < BTREE_MIN_KEYS) return 2;
        if (n.num_keys > BTREE_MAX_KEYS) return 3;
    } else {
        if (n.num_keys > BTREE_MAX_KEYS) return 3;
    }
    for (int i = 1; i < n.num_keys; i++) {
        if (n.keys[i - 1] >= n.keys[i]) return 4;
    }
    for (int i = 0; i < n.num_keys; i++) {
        if (has_lo && n.keys[i] <= lo) return 5;
        if (has_hi && n.keys[i] >= hi) return 6;
    }
    if (n.is_leaf) {
        if (*leaf_depth == -1) *leaf_depth = depth;
        else if (*leaf_depth != depth) return 7;
        return 0;
    }
    for (int i = 0; i <= n.num_keys; i++) {
        if (n.children[i] == 0) return 8;
    }
    for (int i = 0; i <= n.num_keys; i++) {
        btree_key_t cl = lo, ch = hi;
        int hcl = has_lo, hch = has_hi;
        if (i > 0) { cl = n.keys[i - 1]; hcl = 1; }
        if (i < n.num_keys) { ch = n.keys[i]; hch = 1; }
        int rc = check_d(pm, n.children[i], 0, depth + 1, leaf_depth,
                         cl, hcl, ch, hch);
        if (rc != 0) return rc;
    }
    return 0;
}

int dbt_check_invariants(DBTree *t) {
    if (t->pm->hdr.root_page == 0) return 0;
    int leaf_depth = -1;
    return check_d(t->pm, t->pm->hdr.root_page, 1, 0, &leaf_depth,
                   0, 0, 0, 0);
}
