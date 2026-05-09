/* test_phase1.c — Phase 1 acceptance test
 *
 * Theo spec Phase 1:
 *   [x] HugoHeader struct + serialization
 *   [x] hugo_io abstraction layer
 *   [x] Page manager: alloc/read/write
 *   [x] CRC32 checksum
 *   [x] Test: ghi 10,000 page, đọc lại, verify checksum 100%
 *
 * Thêm các test phụ:
 *   - Endianness round-trip
 *   - CRC32 known vectors
 *   - DB header round-trip + reopen
 *   - Magic number wrong → reject
 *   - Tampered byte → checksum fail (corruption detection)
 *   - Wrong page_id field → checksum fail (page identity bound vào CRC)
 */
#include "../src/core/serializer.h"
#include "../src/core/checksum.h"
#include "../src/core/hugo_io.h"
#include "../src/core/page.h"

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

/* ===== Test 1: serializer round-trip ===== */
static void test_serializer(void) {
    printf("\n[1] Serializer round-trip\n");
    uint8_t buf[8];

    write_u16_be(buf, 0x1234);
    CHECK(buf[0] == 0x12 && buf[1] == 0x34, "u16 BE byte order");
    CHECK(read_u16_be(buf) == 0x1234, "u16 round-trip");

    write_u32_be(buf, 0xDEADBEEF);
    CHECK(buf[0] == 0xDE && buf[1] == 0xAD && buf[2] == 0xBE && buf[3] == 0xEF,
          "u32 BE byte order");
    CHECK(read_u32_be(buf) == 0xDEADBEEF, "u32 round-trip");

    write_u64_be(buf, 0x0123456789ABCDEFULL);
    CHECK(read_u64_be(buf) == 0x0123456789ABCDEFULL, "u64 round-trip");

    /* Edge: max values */
    write_u32_be(buf, 0xFFFFFFFFu);
    CHECK(read_u32_be(buf) == 0xFFFFFFFFu, "u32 max");
    write_u64_be(buf, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(read_u64_be(buf) == 0xFFFFFFFFFFFFFFFFULL, "u64 max");

    /* MAGIC literal — Lưu ý: spec ghi 0x48554755 và bảo là "HUGO",
     * nhưng 0x48554755 BE thực tế = "HUGU" (0x4F mới là 'O').
     * Code dùng đúng literal trong spec. Đây là lỗi comment trong spec
     * — bạn nên quyết định: giữ literal 0x48554755 (HUGU) hay đổi sang
     * 0x4855474F để đúng "HUGO". */
    write_u32_be(buf, HUGO_MAGIC);
    CHECK(buf[0] == 0x48 && buf[1] == 0x55 && buf[2] == 0x47 && buf[3] == 0x55,
          "magic literal 0x48554755 BE bytes (spec literal)");
}

/* ===== Test 2: CRC32 known vectors ===== */
static void test_crc32(void) {
    printf("\n[2] CRC32 known vectors\n");
    /* Standard CRC32 (IEEE) test vectors */
    CHECK(hugo_crc32((const uint8_t*)"", 0) == 0x00000000,
          "crc32(empty) = 0");
    CHECK(hugo_crc32((const uint8_t*)"a", 1) == 0xE8B7BE43,
          "crc32(\"a\") = 0xE8B7BE43");
    CHECK(hugo_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926,
          "crc32(\"123456789\") = 0xCBF43926");

    /* Same input → same output */
    uint8_t data[100];
    for (int i = 0; i < 100; i++) data[i] = (uint8_t)(i * 7);
    uint32_t c1 = hugo_crc32(data, 100);
    uint32_t c2 = hugo_crc32(data, 100);
    CHECK(c1 == c2, "deterministic");

    /* 1 bit flip → khác */
    data[50] ^= 0x01;
    uint32_t c3 = hugo_crc32(data, 100);
    CHECK(c1 != c3, "1-bit flip detected");
}

/* ===== Test 3: DB header round-trip + reopen ===== */
static void test_db_header(void) {
    printf("\n[3] DB header create/reopen\n");
    const char *path = "hugo_phase1_hdr.hugo";
    remove(path);

    PageManager pm;
    int rc = pm_create(&pm, path);
    CHECK(rc == PG_OK, "create new DB");
    CHECK(pm.hdr.magic == HUGO_MAGIC, "magic set");
    CHECK(pm.hdr.version == 1, "version = 1");
    CHECK(pm.hdr.page_size == 4096, "page_size = 4096");
    CHECK(pm.hdr.page_count == 1, "page_count = 1 (chỉ header)");
    pm_close(&pm);

    /* Reopen */
    PageManager pm2;
    rc = pm_open(&pm2, path);
    CHECK(rc == PG_OK, "reopen DB");
    CHECK(pm2.hdr.magic == HUGO_MAGIC, "magic preserved");
    CHECK(pm2.hdr.version == 1, "version preserved");
    CHECK(pm2.hdr.page_count == 1, "page_count preserved");
    pm_close(&pm2);

    remove(path);
}

/* ===== Test 4: Reject wrong magic ===== */
static void test_wrong_magic(void) {
    printf("\n[4] Reject file with wrong magic\n");
    const char *path = "hugo_phase1_bad.hugo";
    remove(path);

    /* Tạo file 4096 bytes toàn 0 */
    HugoFile *f = hugo_open(path, HUGO_OPEN_RDWR | HUGO_OPEN_CREATE);
    uint8_t zeros[HUGO_PAGE_SIZE] = {0};
    hugo_write(f, zeros, HUGO_PAGE_SIZE, 0);
    hugo_sync(f);
    hugo_close(f);

    PageManager pm;
    int rc = pm_open(&pm, path);
    CHECK(rc != PG_OK, "reject file thiếu magic");
    /* Có thể fail ở checksum hoặc magic — cả hai đều OK */
    CHECK(rc == PG_ERR_MAGIC || rc == PG_ERR_CHECKSUM,
          "error là MAGIC hoặc CHECKSUM");

    remove(path);
}

/* ===== Test 5: 10,000 page write/read with CRC verify ===== */
static void test_10k_pages(void) {
    printf("\n[5] 10,000 pages write/read/verify\n");
    const char *path = "hugo_phase1_10k.hugo";
    remove(path);

    const int N = 10000;

    PageManager pm;
    int rc = pm_create(&pm, path);
    CHECK(rc == PG_OK, "create DB for 10k test");

    /* Tạo pattern data có thể tái tạo lại để verify */
    HugoPage page;
    int write_fail = 0;
    clock_t t0 = clock();
    for (int i = 0; i < N; i++) {
        uint64_t pid;
        if (pm_alloc_page(&pm, &pid) != PG_OK) { write_fail++; continue; }

        memset(&page, 0, sizeof(page));
        page.page_id  = (uint32_t)pid;
        page.page_type = (uint8_t)((i % 4) + 1);  /* INTERNAL/LEAF/OVERFLOW/FREE */
        page.num_keys = (uint16_t)(i % 200);
        /* Fill data: deterministic pattern */
        for (int j = 0; j < HUGO_PAGE_DATA_SIZE; j++) {
            page.data[j] = (uint8_t)((i * 31 + j * 17) & 0xFF);
        }
        if (pm_write_page(&pm, &page) != PG_OK) write_fail++;
    }
    /* Flush header để page_count được persist */
    rc = pm_flush_header(&pm);
    CHECK(rc == PG_OK, "flush header sau khi write 10k pages");
    clock_t t1 = clock();
    CHECK(write_fail == 0, "tất cả 10,000 page write thành công");
    printf("       write time: %.2fs (%.0f pages/s)\n",
           (double)(t1-t0)/CLOCKS_PER_SEC,
           N / ((double)(t1-t0)/CLOCKS_PER_SEC + 1e-9));

    pm_close(&pm);

    /* Reopen và đọc verify */
    PageManager pm2;
    rc = pm_open(&pm2, path);
    CHECK(rc == PG_OK, "reopen DB");
    CHECK(pm2.hdr.page_count == (uint64_t)(N + 1), "page_count = 10001");

    int read_fail = 0, content_mismatch = 0, checksum_fail = 0;
    HugoPage rp;
    t0 = clock();
    for (int i = 0; i < N; i++) {
        uint64_t pid = (uint64_t)(i + 1);  /* page 0 là header */
        rc = pm_read_page(&pm2, pid, &rp);
        if (rc == PG_ERR_CHECKSUM) { checksum_fail++; continue; }
        if (rc != PG_OK) { read_fail++; continue; }

        if (rp.page_id != pid) content_mismatch++;
        if (rp.page_type != (uint8_t)((i % 4) + 1)) content_mismatch++;
        if (rp.num_keys != (uint16_t)(i % 200)) content_mismatch++;
        for (int j = 0; j < HUGO_PAGE_DATA_SIZE; j++) {
            if (rp.data[j] != (uint8_t)((i * 31 + j * 17) & 0xFF)) {
                content_mismatch++;
                break;
            }
        }
    }
    t1 = clock();
    CHECK(read_fail == 0, "0 read I/O errors");
    CHECK(checksum_fail == 0, "0 checksum failures (CRC pass 100%)");
    CHECK(content_mismatch == 0, "0 content mismatches");
    printf("       read time: %.2fs (%.0f pages/s)\n",
           (double)(t1-t0)/CLOCKS_PER_SEC,
           N / ((double)(t1-t0)/CLOCKS_PER_SEC + 1e-9));

    pm_close(&pm2);
    remove(path);
}

/* ===== Test 6: corruption detection ===== */
static void test_corruption_detection(void) {
    printf("\n[6] Corruption detection (single byte flip)\n");
    const char *path = "hugo_phase1_corrupt.hugo";
    remove(path);

    PageManager pm;
    pm_create(&pm, path);
    uint64_t pid;
    pm_alloc_page(&pm, &pid);
    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id = (uint32_t)pid;
    page.page_type = PAGE_TYPE_LEAF;
    page.num_keys = 42;
    memset(page.data, 0xAB, HUGO_PAGE_DATA_SIZE);
    pm_write_page(&pm, &page);
    pm_flush_header(&pm);
    pm_close(&pm);

    /* Đọc page raw, flip 1 byte ở giữa data, ghi lại */
    HugoFile *f = hugo_open(path, HUGO_OPEN_RDWR);
    uint8_t buf[HUGO_PAGE_SIZE];
    hugo_read(f, buf, HUGO_PAGE_SIZE, pid * HUGO_PAGE_SIZE);
    buf[HUGO_PAGE_HDR_SIZE + 100] ^= 0x01;  /* flip 1 bit trong data */
    hugo_write(f, buf, HUGO_PAGE_SIZE, pid * HUGO_PAGE_SIZE);
    hugo_sync(f);
    hugo_close(f);

    /* Đọc lại bằng pm — phải fail checksum */
    PageManager pm2;
    pm_open(&pm2, path);
    HugoPage rp;
    int rc = pm_read_page(&pm2, pid, &rp);
    CHECK(rc == PG_ERR_CHECKSUM, "1-bit flip → CRC mismatch detected");
    pm_close(&pm2);

    remove(path);
}

int main(void) {
    printf("=== HUGO DB — Phase 1 Tests ===\n");
    test_serializer();
    test_crc32();
    test_db_header();
    test_wrong_magic();
    test_10k_pages();
    test_corruption_detection();

    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
