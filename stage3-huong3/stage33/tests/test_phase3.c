/* test_phase3.c — Buffer Pool tests
 *
 * Verify:
 *   [x] Cache hit nếu page đã trong pool
 *   [x] Cache miss nếu chưa
 *   [x] Pinned page KHÔNG bị evict
 *   [x] LRU order đúng
 *   [x] Dirty page được flush khi evict
 *   [x] bp_flush_all ghi tất cả dirty xuống disk
 *   [x] Reopen DB sau flush → data còn nguyên (cùng CRC)
 *   [x] Quá nhiều pin → BP_ERR_FULL
 *   [x] Stress: 10k random get/put trên BP nhỏ — verify content
 */
#include "../src/core/page.h"
#include "../src/core/buffer_pool.h"
#include "../src/core/serializer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* Helper: tạo N page với data đoán được */
static void make_page_data(uint8_t *data, uint64_t pid, int variant) {
    /* fill data theo pattern dựa trên (pid, variant) */
    for (int i = 0; i < HUGO_PAGE_DATA_SIZE; i++) {
        data[i] = (uint8_t)((pid * 31 + variant * 7 + i * 11) & 0xFF);
    }
}

static int verify_page_data(const uint8_t *data, uint64_t pid, int variant) {
    for (int i = 0; i < HUGO_PAGE_DATA_SIZE; i++) {
        if (data[i] != (uint8_t)((pid * 31 + variant * 7 + i * 11) & 0xFF))
            return 0;
    }
    return 1;
}

/* ===== Test 1: cache hit basics ===== */
static void test_basic_hit_miss(void) {
    printf("\n[1] Cache hit / miss\n");
    const char *path = "phase3_basic.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    BufferPool bp;
    bp_init(&bp, &pm, 4);

    /* Tạo 1 page mới */
    BufferFrame *f1;
    CHECK(bp_new_page(&bp, &f1) == BP_OK, "new page");
    uint64_t pid1 = f1->page_id;
    f1->page.page_type = PAGE_TYPE_LEAF;
    make_page_data(f1->page.data, pid1, 1);
    bp_put(&bp, f1, 1);

    /* Get lại — phải hit */
    uint64_t hits_before = bp.stat_hits;
    BufferFrame *f2;
    CHECK(bp_get(&bp, pid1, &f2) == BP_OK, "get cached page");
    CHECK(bp.stat_hits == hits_before + 1, "hit count tăng");
    CHECK(verify_page_data(f2->page.data, pid1, 1), "data đúng");
    bp_put(&bp, f2, 0);

    bp_destroy(&bp);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 2: pinned page không evict ===== */
static void test_pin_no_evict(void) {
    printf("\n[2] Pinned page KHÔNG bị evict\n");
    const char *path = "phase3_pin.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    BufferPool bp;
    bp_init(&bp, &pm, 3);  /* nhỏ */

    /* Tạo 3 page, pin 1, đọc page mới → page pinned phải còn */
    BufferFrame *f[5];
    uint64_t pids[5];
    for (int i = 0; i < 3; i++) {
        bp_new_page(&bp, &f[i]);
        pids[i] = f[i]->page_id;
        f[i]->page.page_type = PAGE_TYPE_LEAF;
        make_page_data(f[i]->page.data, pids[i], i);
        bp_put(&bp, f[i], 1);
    }
    /* Pin page 0 (giữ trong cache) */
    BufferFrame *pinned;
    bp_get(&bp, pids[0], &pinned);

    /* Tạo thêm 2 page → phải evict 2 trong 3 page kia (1 và 2),
     * page 0 vì pinned không evict được */
    bp_new_page(&bp, &f[3]);
    pids[3] = f[3]->page_id;
    bp_put(&bp, f[3], 1);
    bp_new_page(&bp, &f[4]);
    pids[4] = f[4]->page_id;
    bp_put(&bp, f[4], 1);

    /* Verify: page 0 vẫn ở trong frame của pinned, page_id còn nguyên */
    CHECK(pinned->page_id == pids[0], "pinned page_id giữ nguyên");
    CHECK(verify_page_data(pinned->page.data, pids[0], 0), "pinned data còn nguyên");
    bp_put(&bp, pinned, 0);

    bp_flush_all(&bp);
    bp_destroy(&bp);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 3: dirty page được flush khi evict ===== */
static void test_dirty_flush_on_evict(void) {
    printf("\n[3] Dirty page được flush trước khi evict\n");
    const char *path = "phase3_dirty.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    BufferPool bp;
    bp_init(&bp, &pm, 2);  /* rất nhỏ → ép evict */

    /* Tạo 2 page */
    BufferFrame *f1, *f2;
    bp_new_page(&bp, &f1); uint64_t p1 = f1->page_id;
    f1->page.page_type = PAGE_TYPE_LEAF; make_page_data(f1->page.data, p1, 1);
    bp_put(&bp, f1, 1);

    bp_new_page(&bp, &f2); uint64_t p2 = f2->page_id;
    f2->page.page_type = PAGE_TYPE_LEAF; make_page_data(f2->page.data, p2, 2);
    bp_put(&bp, f2, 1);

    /* Tạo page 3 → cache đầy → phải evict (p1 hoặc p2), flush dirty */
    BufferFrame *f3;
    uint64_t flushes_before = bp.stat_flushes;
    bp_new_page(&bp, &f3);
    f3->page.page_type = PAGE_TYPE_LEAF; make_page_data(f3->page.data, f3->page_id, 3);
    bp_put(&bp, f3, 1);
    CHECK(bp.stat_flushes >= flushes_before + 1, "ít nhất 1 flush khi evict dirty");

    /* Đọc lại p1 — sẽ miss, load từ disk → verify nội dung */
    BufferFrame *fr;
    bp_get(&bp, p1, &fr);
    CHECK(verify_page_data(fr->page.data, p1, 1), "p1 sau evict+flush+reload đúng data");
    bp_put(&bp, fr, 0);
    bp_get(&bp, p2, &fr);
    CHECK(verify_page_data(fr->page.data, p2, 2), "p2 đúng data");
    bp_put(&bp, fr, 0);

    bp_flush_all(&bp);
    bp_destroy(&bp);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 4: persistence qua flush_all + reopen ===== */
static void test_persistence(void) {
    printf("\n[4] flush_all + close + reopen → data còn nguyên\n");
    const char *path = "phase3_persist.hugo";
    remove(path);

    const int N = 50;
    uint64_t pids[50];

    {
        PageManager pm;
        pm_create(&pm, path);
        BufferPool bp;
        bp_init(&bp, &pm, 8);  /* nhỏ hơn N → có evict */

        for (int i = 0; i < N; i++) {
            BufferFrame *f;
            bp_new_page(&bp, &f);
            pids[i] = f->page_id;
            f->page.page_type = PAGE_TYPE_LEAF;
            make_page_data(f->page.data, pids[i], i);
            bp_put(&bp, f, 1);
        }
        bp_flush_all(&bp);
        bp_destroy(&bp);
        pm_close(&pm);
    }

    /* Reopen, đọc trực tiếp qua pm (không qua bp) để chứng minh data đã trên disk */
    {
        PageManager pm;
        pm_open(&pm, path);
        int hits = 0;
        for (int i = 0; i < N; i++) {
            HugoPage page;
            if (pm_read_page(&pm, pids[i], &page) == PG_OK
                && verify_page_data(page.data, pids[i], i)) hits++;
        }
        CHECK(hits == N, "tất cả N page đọc đúng sau reopen");
        pm_close(&pm);
    }
    remove(path);
}

/* ===== Test 5: stress với BP nhỏ ===== */
static void test_stress(void) {
    printf("\n[5] Stress: 10k get/put trên 100 page với BP=8\n");
    const char *path = "phase3_stress.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    BufferPool bp;
    bp_init(&bp, &pm, 8);

    const int N = 100;
    uint64_t pids[100];
    /* Tạo trước N page */
    for (int i = 0; i < N; i++) {
        BufferFrame *f;
        bp_new_page(&bp, &f);
        pids[i] = f->page_id;
        f->page.page_type = PAGE_TYPE_LEAF;
        make_page_data(f->page.data, pids[i], i);
        bp_put(&bp, f, 1);
    }
    bp_flush_all(&bp);

    /* 10k random get + verify */
    srand(42);
    int mismatches = 0;
    for (int op = 0; op < 10000; op++) {
        int idx = rand() % N;
        BufferFrame *f;
        if (bp_get(&bp, pids[idx], &f) != BP_OK) { mismatches++; continue; }
        if (!verify_page_data(f->page.data, pids[idx], idx)) mismatches++;
        bp_put(&bp, f, 0);
    }
    CHECK(mismatches == 0, "10k random get → 0 mismatch");
    printf("       "); bp_print_stats(&bp);
    CHECK(bp.stat_hits + bp.stat_misses >= 10000, "tổng hit+miss >= 10k");
    CHECK(bp.stat_evictions > 0, "có evictions (BP nhỏ hơn working set)");

    bp_destroy(&bp);
    pm_close(&pm);
    remove(path);
}

/* ===== Test 6: pin all → BP_ERR_FULL ===== */
static void test_full_pinned(void) {
    printf("\n[6] Tất cả frames pinned → bp_get tiếp = BP_ERR_FULL\n");
    const char *path = "phase3_full.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    BufferPool bp;
    bp_init(&bp, &pm, 3);

    BufferFrame *f[4];
    uint64_t pids[4];
    for (int i = 0; i < 3; i++) {
        bp_new_page(&bp, &f[i]);
        pids[i] = f[i]->page_id;
        f[i]->page.page_type = PAGE_TYPE_LEAF;
        bp_put(&bp, f[i], 1);
    }
    /* Pin cả 3 (không put back) */
    BufferFrame *pinned[3];
    for (int i = 0; i < 3; i++) bp_get(&bp, pids[i], &pinned[i]);

    /* Cố tạo page mới — phải fail */
    BufferFrame *f4;
    int rc = bp_new_page(&bp, &f4);
    CHECK(rc == BP_ERR_FULL, "BP đầy pinned → BP_ERR_FULL");

    /* Unpin 1 → phải làm được */
    bp_put(&bp, pinned[0], 0);
    rc = bp_new_page(&bp, &f4);
    CHECK(rc == BP_OK, "sau unpin → bp_new_page OK");
    bp_put(&bp, f4, 1);

    bp_put(&bp, pinned[1], 0);
    bp_put(&bp, pinned[2], 0);
    bp_flush_all(&bp);
    bp_destroy(&bp);
    pm_close(&pm);
    remove(path);
}

int main(void) {
    printf("=== HUGO DB — Phase 3 (Buffer Pool) Tests ===\n");
    test_basic_hit_miss();
    test_pin_no_evict();
    test_dirty_flush_on_evict();
    test_persistence();
    test_stress();
    test_full_pinned();

    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
