/* test_phase2.c — B-tree property-based test
 *
 * Phase 2 acceptance:
 *   [x] insert / search / delete trên RAM
 *   [x] Property-based: random ops, verify invariants sau MỖI op
 *   [x] Corner cases: split root, merge, redistribute
 *   [x] ASCII visualizer
 *   [ ] Map xuống disk (Phase 2.b — sẽ làm sau khi RAM đã chắc)
 *
 * Strategy: dùng "model" = sorted array (đã được simulate qua sort của
 * bộ insert) làm ground truth, so sánh với B-tree.
 */
#include "../src/core/btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* ===== Simple in-memory model: sorted array + values ===== */
typedef struct {
    btree_key_t *keys;
    btree_val_t *vals;
    int          n;
    int          cap;
} Model;

static Model* model_new(int cap) {
    Model *m = calloc(1, sizeof(Model));
    m->cap = cap;
    m->keys = malloc(sizeof(btree_key_t) * cap);
    m->vals = malloc(sizeof(btree_val_t) * cap);
    return m;
}
static void model_free(Model *m) { free(m->keys); free(m->vals); free(m); }

static int model_find(const Model *m, btree_key_t k) {
    /* Binary search */
    int lo = 0, hi = m->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (m->keys[mid] < k) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->n && m->keys[lo] == k) return lo;
    return -1;
}

static int model_insert(Model *m, btree_key_t k, btree_val_t v) {
    int lo = 0, hi = m->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (m->keys[mid] < k) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->n && m->keys[lo] == k) return -1;  /* dup */
    for (int i = m->n; i > lo; i--) {
        m->keys[i] = m->keys[i-1];
        m->vals[i] = m->vals[i-1];
    }
    m->keys[lo] = k;
    m->vals[lo] = v;
    m->n++;
    return 0;
}

static int model_delete(Model *m, btree_key_t k) {
    int idx = model_find(m, k);
    if (idx < 0) return -1;
    for (int i = idx; i < m->n - 1; i++) {
        m->keys[i] = m->keys[i+1];
        m->vals[i] = m->vals[i+1];
    }
    m->n--;
    return 0;
}

/* ===== Test 1: basic insert + search ===== */
static void test_basic(void) {
    printf("\n[1] Basic insert + search\n");
    BTree *t = btree_create();

    CHECK(btree_insert(t, 10, 100) == BT_OK, "insert 10");
    CHECK(btree_insert(t, 20, 200) == BT_OK, "insert 20");
    CHECK(btree_insert(t, 5,  50)  == BT_OK, "insert 5");
    CHECK(btree_insert(t, 10, 999) == BT_DUP, "duplicate rejected");

    btree_val_t v;
    CHECK(btree_search(t, 10, &v) == BT_OK && v == 100, "search 10 → 100");
    CHECK(btree_search(t, 20, &v) == BT_OK && v == 200, "search 20 → 200");
    CHECK(btree_search(t, 5,  &v) == BT_OK && v == 50,  "search 5 → 50");
    CHECK(btree_search(t, 99, &v) == BT_NOT_FOUND, "search 99 → not found");

    CHECK(btree_check_invariants(t) == 0, "invariants OK");
    CHECK(t->count == 3, "count = 3");

    btree_destroy(t);
}

/* ===== Test 2: split root (insert 2t-1+1 = 2t keys tăng dần) ===== */
static void test_split_root(void) {
    printf("\n[2] Split root\n");
    BTree *t = btree_create();
    for (int i = 1; i <= 2 * BTREE_T; i++) {
        int rc = btree_insert(t, (btree_key_t)i, (btree_val_t)(i * 10));
        if (rc != BT_OK) { printf("  insert %d failed: %d\n", i, rc); }
        CHECK(btree_check_invariants(t) == 0, "invariants OK after each insert");
    }
    /* Sau 2t inserts, root phải đã split → cây có depth 1 */
    CHECK(!t->root->is_leaf, "root đã thành internal sau 2t inserts");
    btree_destroy(t);
}

/* ===== Test 3: ascending insert N keys, verify all ===== */
static void test_ascending(void) {
    printf("\n[3] Ascending insert (worst case for split)\n");
    BTree *t = btree_create();
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        int rc = btree_insert(t, (btree_key_t)i, (btree_val_t)(i + 1));
        if (rc != BT_OK) { printf("  insert %d failed\n", i); break; }
    }
    CHECK(btree_check_invariants(t) == 0, "invariants OK after 1000 ascending");
    CHECK((int)t->count == N, "count = N");
    int hits = 0;
    for (int i = 0; i < N; i++) {
        btree_val_t v;
        if (btree_search(t, (btree_key_t)i, &v) == BT_OK && v == (btree_val_t)(i+1))
            hits++;
    }
    CHECK(hits == N, "all 1000 keys found with correct values");
    btree_destroy(t);
}

/* ===== Test 4: descending insert ===== */
static void test_descending(void) {
    printf("\n[4] Descending insert\n");
    BTree *t = btree_create();
    const int N = 1000;
    for (int i = N; i > 0; i--) {
        btree_insert(t, (btree_key_t)i, (btree_val_t)(i * 2));
    }
    CHECK(btree_check_invariants(t) == 0, "invariants OK after descending");
    int hits = 0;
    for (int i = 1; i <= N; i++) {
        btree_val_t v;
        if (btree_search(t, (btree_key_t)i, &v) == BT_OK && v == (btree_val_t)(i*2))
            hits++;
    }
    CHECK(hits == N, "all keys found");
    btree_destroy(t);
}

/* ===== Test 5: delete leaf cases (no rebalance needed) ===== */
static void test_delete_simple(void) {
    printf("\n[5] Simple delete\n");
    BTree *t = btree_create();
    for (int i = 1; i <= 20; i++) btree_insert(t, i, i * 10);

    CHECK(btree_delete(t, 5) == BT_OK, "delete 5");
    CHECK(btree_check_invariants(t) == 0, "invariants OK");
    btree_val_t v;
    CHECK(btree_search(t, 5, &v) == BT_NOT_FOUND, "5 gone");
    CHECK(btree_search(t, 10, &v) == BT_OK && v == 100, "10 vẫn còn");

    CHECK(btree_delete(t, 999) == BT_NOT_FOUND, "delete inexistent");

    btree_destroy(t);
}

/* ===== Test 6: delete tất cả → empty ===== */
static void test_delete_all(void) {
    printf("\n[6] Delete all keys → tree empty\n");
    BTree *t = btree_create();
    const int N = 500;
    for (int i = 0; i < N; i++) btree_insert(t, i, i);

    /* Xoá theo thứ tự ngẫu nhiên xen kẽ */
    int order[500];
    for (int i = 0; i < N; i++) order[i] = i;
    /* Shuffle */
    srand(12345);
    for (int i = N - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }

    int inv_fails = 0;
    for (int i = 0; i < N; i++) {
        int rc = btree_delete(t, order[i]);
        if (rc != BT_OK) { printf("  delete %d failed\n", order[i]); }
        if (btree_check_invariants(t) != 0) inv_fails++;
    }
    CHECK(inv_fails == 0, "invariants OK sau mỗi delete (500 ops)");
    CHECK(t->count == 0, "count = 0");
    btree_val_t v;
    CHECK(btree_search(t, 0, &v) == BT_NOT_FOUND, "search trên cây rỗng → not found");

    btree_destroy(t);
}

/* ===== Test 7: PROPERTY-BASED — random ops, verify mỗi step ===== */
static void test_property_based(int n_ops, unsigned seed) {
    printf("\n[7] Property-based: %d random ops, seed=%u\n", n_ops, seed);
    BTree *t = btree_create();
    Model *m = model_new(n_ops + 100);

    srand(seed);
    int inv_fails = 0;
    int divergences = 0;

    for (int op = 0; op < n_ops; op++) {
        int action = rand() % 100;
        btree_key_t k = (btree_key_t)(rand() % (n_ops / 2 + 1));  /* nhiều dup */

        if (action < 60) {
            /* Insert */
            btree_val_t v = (btree_val_t)(k * 1000 + op);
            int br = btree_insert(t, k, v);
            int mr = model_insert(m, k, v);
            if ((br == BT_OK) != (mr == 0)) divergences++;
        } else if (action < 80) {
            /* Search */
            btree_val_t bv;
            int br = btree_search(t, k, &bv);
            int mi = model_find(m, k);
            if ((br == BT_OK) != (mi >= 0)) divergences++;
            if (br == BT_OK && mi >= 0 && bv != m->vals[mi]) divergences++;
        } else {
            /* Delete */
            int br = btree_delete(t, k);
            int mr = model_delete(m, k);
            if ((br == BT_OK) != (mr == 0)) divergences++;
        }

        if (btree_check_invariants(t) != 0) {
            inv_fails++;
            if (inv_fails == 1) {
                printf("  First invariant fail at op %d (action=%d, k=%llu)\n",
                       op, action, (unsigned long long)k);
            }
        }
        if ((int)t->count != m->n) divergences++;
    }

    /* Final check: tất cả keys trong model đều tìm được trong tree */
    int find_fails = 0;
    for (int i = 0; i < m->n; i++) {
        btree_val_t v;
        if (btree_search(t, m->keys[i], &v) != BT_OK || v != m->vals[i]) find_fails++;
    }

    CHECK(inv_fails == 0,   "invariants OK sau mỗi op");
    CHECK(divergences == 0, "tree behavior khớp model");
    CHECK(find_fails == 0,  "final state: tất cả keys trong model tìm được");

    btree_destroy(t);
    model_free(m);
}

/* ===== Test 8: ASCII visualizer demo ===== */
static void test_visualizer(void) {
    printf("\n[8] ASCII visualizer (demo, không assert)\n");
    BTree *t = btree_create();
    for (int i = 0; i < 15; i++) btree_insert(t, i * 3, i);
    btree_print(t);
    btree_destroy(t);
    tests_run++;
    tests_passed++;
    printf("  ok  : visualizer chạy không crash\n");
}

int main(void) {
    printf("=== HUGO DB — Phase 2 (B-tree RAM) Tests ===\n");
    printf("BTREE_T = %d, MAX_KEYS = %d, MIN_KEYS = %d\n",
           BTREE_T, BTREE_MAX_KEYS, BTREE_MIN_KEYS);

    test_basic();
    test_split_root();
    test_ascending();
    test_descending();
    test_delete_simple();
    test_delete_all();

    /* Property-based với nhiều seed khác nhau */
    test_property_based(2000,  1);
    test_property_based(2000,  42);
    test_property_based(2000,  9999);
    test_property_based(10000, 7);

    test_visualizer();

    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
