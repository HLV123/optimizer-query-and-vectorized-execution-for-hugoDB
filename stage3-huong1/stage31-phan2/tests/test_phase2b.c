/* test_phase2b.c — Disk B-tree test
 *
 * Phase 2.b acceptance:
 *   [x] B-tree map xuống disk page
 *   [x] insert/search/delete với data persisted
 *   [x] Reopen DB → tree còn nguyên
 *   [x] Property-based: so với RAM B-tree (cùng input → cùng kết quả)
 *   [x] CRC verify khi đọc page → nếu disk B-tree pass tức page checksum cũng pass
 */
#include "../src/core/btree.h"
#include "../src/core/dbtree.h"
#include "../src/core/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* ===== Test 1: basic ===== */
static void test_basic(void) {
    printf("\n[1] Basic disk insert + search\n");
    const char *path = "phase2b_basic.hugo";
    remove(path);

    PageManager pm;
    CHECK(pm_create(&pm, path) == PG_OK, "create DB");

    DBTree dt;
    dbt_init(&dt, &pm);

    CHECK(dbt_insert(&dt, 10, 100) == BT_OK, "insert 10");
    CHECK(dbt_insert(&dt, 20, 200) == BT_OK, "insert 20");
    CHECK(dbt_insert(&dt, 5,  50)  == BT_OK, "insert 5");
    CHECK(dbt_insert(&dt, 10, 999) == BT_DUP, "duplicate rejected");

    btree_val_t v;
    CHECK(dbt_search(&dt, 10, &v) == BT_OK && v == 100, "search 10 = 100");
    CHECK(dbt_search(&dt, 20, &v) == BT_OK && v == 200, "search 20 = 200");
    CHECK(dbt_search(&dt, 5,  &v) == BT_OK && v == 50,  "search 5 = 50");
    CHECK(dbt_search(&dt, 99, &v) == BT_NOT_FOUND, "search 99 not found");

    CHECK(dbt_check_invariants(&dt) == 0, "invariants OK");
    dbt_close(&dt);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 2: persistence (close + reopen) ===== */
static void test_persist(void) {
    printf("\n[2] Persistence: close + reopen → data còn nguyên\n");
    const char *path = "phase2b_persist.hugo";
    remove(path);

    {
        PageManager pm;
        pm_create(&pm, path);
        DBTree dt;
        dbt_init(&dt, &pm);
        for (int i = 0; i < 100; i++) {
            dbt_insert(&dt, (btree_key_t)i, (btree_val_t)(i * 7 + 1));
        }
        CHECK(dbt_check_invariants(&dt) == 0, "invariants OK trước close");
        dbt_close(&dt);
        pm_close(&pm);
    }

    /* Reopen */
    {
        PageManager pm;
        CHECK(pm_open(&pm, path) == PG_OK, "reopen DB");
        DBTree dt;
        dbt_init(&dt, &pm);
        CHECK(dbt_check_invariants(&dt) == 0, "invariants OK sau reopen");

        int hits = 0;
        for (int i = 0; i < 100; i++) {
            btree_val_t v;
            if (dbt_search(&dt, (btree_key_t)i, &v) == BT_OK
                && v == (btree_val_t)(i * 7 + 1)) hits++;
        }
        CHECK(hits == 100, "tất cả 100 keys persistent với value đúng");

        dbt_close(&dt);
        pm_close(&pm);
    }
    remove(path);
}

/* ===== Test 3: ascending ===== */
static void test_ascending(void) {
    printf("\n[3] Ascending insert 500 keys\n");
    const char *path = "phase2b_asc.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    DBTree dt;
    dbt_init(&dt, &pm);

    for (int i = 0; i < 500; i++) dbt_insert(&dt, i, i * 2);
    CHECK(dbt_check_invariants(&dt) == 0, "invariants OK");
    int hits = 0;
    for (int i = 0; i < 500; i++) {
        btree_val_t v;
        if (dbt_search(&dt, i, &v) == BT_OK && v == (btree_val_t)(i * 2)) hits++;
    }
    CHECK(hits == 500, "all 500 found");

    dbt_close(&dt);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 4: delete + persistence ===== */
static void test_delete_persist(void) {
    printf("\n[4] Delete + close + reopen → deletion persist\n");
    const char *path = "phase2b_del.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    DBTree dt;
    dbt_init(&dt, &pm);

    for (int i = 0; i < 50; i++) dbt_insert(&dt, i, i + 100);
    /* Xoá keys chẵn */
    for (int i = 0; i < 50; i += 2) dbt_delete(&dt, i);
    CHECK(dbt_check_invariants(&dt) == 0, "invariants OK sau xoá");

    dbt_close(&dt);
    pm_close(&pm);

    pm_open(&pm, path);
    dbt_init(&dt, &pm);
    int odd_found = 0, even_gone = 0;
    for (int i = 0; i < 50; i++) {
        btree_val_t v;
        int rc = dbt_search(&dt, i, &v);
        if (i % 2 == 1 && rc == BT_OK && v == (btree_val_t)(i + 100)) odd_found++;
        if (i % 2 == 0 && rc == BT_NOT_FOUND) even_gone++;
    }
    CHECK(odd_found == 25, "25 odd keys vẫn còn sau reopen");
    CHECK(even_gone == 25, "25 even keys đã xoá sau reopen");

    dbt_close(&dt);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 5: PROPERTY-BASED — disk vs RAM ===== */
static void test_disk_vs_ram(int n_ops, unsigned seed) {
    printf("\n[5] Disk B-tree khớp RAM B-tree (%d ops, seed=%u)\n", n_ops, seed);
    const char *path = "phase2b_cmp.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    DBTree dt;
    dbt_init(&dt, &pm);

    BTree *rt = btree_create();

    srand(seed);
    int divergence = 0, inv_fail = 0;
    int check_every = (n_ops > 1000) ? (n_ops / 50) : 50;

    for (int i = 0; i < n_ops; i++) {
        int action = rand() % 100;
        btree_key_t k = (btree_key_t)(rand() % (n_ops / 2 + 1));

        if (action < 60) {
            btree_val_t v = (btree_val_t)(k * 1000 + i);
            int dr = dbt_insert(&dt, k, v);
            int rr = btree_insert(rt, k, v);
            if (dr != rr) divergence++;
        } else if (action < 80) {
            btree_val_t dv, rv;
            int dr = dbt_search(&dt, k, &dv);
            int rr = btree_search(rt, k, &rv);
            if (dr != rr) divergence++;
            if (dr == BT_OK && rr == BT_OK && dv != rv) divergence++;
        } else {
            int dr = dbt_delete(&dt, k);
            int rr = btree_delete(rt, k);
            if (dr != rr) divergence++;
        }

        if ((i + 1) % check_every == 0) {
            if (dbt_check_invariants(&dt) != 0) inv_fail++;
            if (btree_check_invariants(rt) != 0) inv_fail++;
        }
    }

    /* Final: tất cả keys trong RAM phải tìm được trên disk với value khớp */
    int find_fail = 0;
    /* Walk RAM tree bằng cách enumerate keys 0..n_ops/2 */
    for (int k = 0; k <= n_ops / 2; k++) {
        btree_val_t rv, dv;
        int rr = btree_search(rt, (btree_key_t)k, &rv);
        int dr = dbt_search(&dt, (btree_key_t)k, &dv);
        if (rr != dr) find_fail++;
        if (rr == BT_OK && dr == BT_OK && rv != dv) find_fail++;
    }

    CHECK(divergence == 0, "behavior khớp RAM tree");
    CHECK(inv_fail == 0, "invariants OK định kỳ");
    CHECK(find_fail == 0, "final state khớp RAM tree");
    CHECK(dbt_check_invariants(&dt) == 0, "final disk invariants OK");

    btree_destroy(rt);
    dbt_close(&dt);
    pm_close(&pm);
    remove(path);
}

int main(void) {
    printf("=== HUGO DB — Phase 2.b (Disk B-tree) Tests ===\n");
    test_basic();
    test_persist();
    test_ascending();
    test_delete_persist();
    test_disk_vs_ram(500,  1);
    test_disk_vs_ram(2000, 42);
    test_disk_vs_ram(5000, 7);

    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
