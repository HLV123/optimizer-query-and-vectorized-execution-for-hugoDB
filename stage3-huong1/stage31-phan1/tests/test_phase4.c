/* test_phase4.c — WAL + ARIES crash recovery
 *
 * Phase 4 acceptance:
 *   [x] WAL record format + serialization
 *   [x] Append + iterate
 *   [x] CRC verify mỗi record
 *   [x] Crash simulation: flush WAL nhưng KHÔNG flush data → recovery REDO
 *   [x] Loser tx (BEGIN không COMMIT) → recovery UNDO
 *   [x] Truncated tail (crash giữa write) → vẫn parse được phần valid
 */
#include "../src/core/page.h"
#include "../src/core/wal.h"
#include "../src/core/serializer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* ===== Test 1: append + iterate basic ===== */
static void test_append_iterate(void) {
    printf("\n[1] Append + iterate WAL records\n");
    const char *log = "phase4_basic.hugolog";
    remove(log);

    Wal w;
    CHECK(wal_open(&w, log) == WAL_OK, "open empty WAL");
    CHECK(w.next_lsn == 1, "next_lsn = 1 cho WAL trống");

    uint64_t tx = wal_new_tx_id(&w);
    int64_t lsn1 = wal_log_begin(&w, tx);
    CHECK(lsn1 > 0, "log BEGIN");

    uint8_t before[10] = "AAAAAAAAAA";
    uint8_t after[10]  = "BBBBBBBBBB";
    int64_t lsn2 = wal_log_update(&w, tx, /*pid=*/5, /*off=*/100,
                                   before, after, 10);
    CHECK(lsn2 == lsn1 + 1, "lsn UPDATE = BEGIN+1");

    int64_t lsn3 = wal_log_commit(&w, tx);
    CHECK(lsn3 == lsn2 + 1, "lsn COMMIT = UPDATE+1");
    CHECK(wal_sync(&w) == WAL_OK, "sync");

    /* Iterate lại */
    WalIter it;
    wal_iter_init(&it, &w);
    WalRecord rec;
    int n = 0, types[3] = {0};
    while (wal_iter_next(&it, &rec) == WAL_OK) {
        if (n < 3) types[n] = rec.type;
        n++;
    }
    CHECK(n == 3, "iterate đủ 3 records");
    CHECK(types[0] == WAL_BEGIN && types[1] == WAL_UPDATE && types[2] == WAL_COMMIT,
          "thứ tự BEGIN, UPDATE, COMMIT");

    wal_close(&w);
    remove(log);
}

/* ===== Test 2: reopen WAL → next_lsn tiếp tục ===== */
static void test_reopen(void) {
    printf("\n[2] Reopen WAL → next_lsn / next_tx_id tiếp tục\n");
    const char *log = "phase4_reopen.hugolog";
    remove(log);

    {
        Wal w;
        wal_open(&w, log);
        uint64_t tx = wal_new_tx_id(&w);
        wal_log_begin(&w, tx);
        wal_log_commit(&w, tx);
        wal_sync(&w);
        wal_close(&w);
    }
    {
        Wal w;
        wal_open(&w, log);
        CHECK(w.next_lsn == 3, "next_lsn = 3 sau 2 records");
        CHECK(w.next_tx_id == 2, "next_tx_id = 2 sau tx 1");
        wal_close(&w);
    }
    remove(log);
}

/* ===== Test 3: CRASH RECOVERY — REDO ===== */
static void test_redo(void) {
    printf("\n[3] Crash recovery REDO: WAL persisted nhưng data chưa flush\n");
    const char *db  = "phase4_redo.hugo";
    const char *log = "phase4_redo.hugolog";
    remove(db); remove(log);

    /* Setup: tạo DB với 1 page, fill data ban đầu = 0xAA */
    PageManager pm;
    pm_create(&pm, db);
    uint64_t pid;
    pm_alloc_page(&pm, &pid);
    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id = (uint32_t)pid;
    page.page_type = PAGE_TYPE_LEAF;
    memset(page.data, 0xAA, HUGO_PAGE_DATA_SIZE);
    pm_write_page(&pm, &page);
    pm_flush_header(&pm);

    /* SIMULATE: tx ghi WAL update với after_image = 0xBB tại offset 100,
     * length 50. WAL được sync. NHƯNG data page KHÔNG ghi xuống disk
     * (simulate crash trước khi BP flush). */
    Wal w;
    wal_open(&w, log);
    uint64_t tx = wal_new_tx_id(&w);
    wal_log_begin(&w, tx);

    uint8_t before[50], after[50];
    memset(before, 0xAA, 50);
    memset(after,  0xBB, 50);
    wal_log_update(&w, tx, pid, 100, before, after, 50);
    wal_log_commit(&w, tx);
    wal_sync(&w);            /* WAL đã trên disk */
    /* KHÔNG ghi page → simulate crash */
    wal_close(&w);
    pm_close(&pm);

    /* RECOVERY: mở lại */
    pm_open(&pm, db);
    HugoPage check;
    pm_read_page(&pm, pid, &check);
    /* Trước recovery: data vẫn 0xAA */
    int aa_before = 1;
    for (int i = 100; i < 150; i++) if (check.data[i] != 0xAA) { aa_before = 0; break; }
    CHECK(aa_before, "trước recovery: data vẫn 0xAA (chưa REDO)");

    wal_open(&w, log);
    int rc = wal_recover(&w, &pm);
    CHECK(rc == WAL_OK, "recovery thành công");

    pm_read_page(&pm, pid, &check);
    int bb_after = 1;
    for (int i = 100; i < 150; i++) if (check.data[i] != 0xBB) { bb_after = 0; break; }
    CHECK(bb_after, "sau recovery: data = 0xBB (REDO applied)");
    /* Bytes ngoài range vẫn 0xAA */
    int outside_ok = 1;
    for (int i = 0; i < 100; i++) if (check.data[i] != 0xAA) { outside_ok = 0; break; }
    for (int i = 150; i < 200; i++) if (check.data[i] != 0xAA) { outside_ok = 0; break; }
    CHECK(outside_ok, "bytes ngoài update range không bị thay đổi");

    wal_close(&w);
    pm_close(&pm);
    remove(db); remove(log);
}

/* ===== Test 4: UNDO loser tx ===== */
static void test_undo(void) {
    printf("\n[4] Crash recovery UNDO: tx có BEGIN+UPDATE nhưng KHÔNG COMMIT\n");
    const char *db  = "phase4_undo.hugo";
    const char *log = "phase4_undo.hugolog";
    remove(db); remove(log);

    PageManager pm;
    pm_create(&pm, db);
    uint64_t pid;
    pm_alloc_page(&pm, &pid);
    HugoPage page;
    memset(&page, 0, sizeof(page));
    page.page_id = (uint32_t)pid;
    page.page_type = PAGE_TYPE_LEAF;
    memset(page.data, 0xCC, HUGO_PAGE_DATA_SIZE);
    pm_write_page(&pm, &page);
    pm_flush_header(&pm);

    /* Tình huống xấu: tx BEGIN, UPDATE, FLUSH page xuống disk, rồi crash
     * trước khi COMMIT. Recovery phải UNDO (rollback). */
    Wal w;
    wal_open(&w, log);
    uint64_t tx = wal_new_tx_id(&w);
    wal_log_begin(&w, tx);

    uint8_t before[30], after[30];
    memset(before, 0xCC, 30);
    memset(after,  0xDD, 30);
    wal_log_update(&w, tx, pid, 200, before, after, 30);
    wal_sync(&w);

    /* Apply page change (simulate writer làm điều này TRƯỚC commit) */
    memset(page.data + 200, 0xDD, 30);
    pm_write_page(&pm, &page);
    /* CRASH trước commit */
    wal_close(&w);
    pm_close(&pm);

    /* Recovery */
    pm_open(&pm, db);
    HugoPage check;
    pm_read_page(&pm, pid, &check);
    int dd_before = 1;
    for (int i = 200; i < 230; i++) if (check.data[i] != 0xDD) { dd_before = 0; break; }
    CHECK(dd_before, "trước recovery: data có 0xDD (uncommitted)");

    wal_open(&w, log);
    wal_recover(&w, &pm);

    pm_read_page(&pm, pid, &check);
    int cc_after = 1;
    for (int i = 200; i < 230; i++) if (check.data[i] != 0xCC) { cc_after = 0; break; }
    CHECK(cc_after, "sau recovery: data rolled back về 0xCC (UNDO)");

    wal_close(&w);
    pm_close(&pm);
    remove(db); remove(log);
}

/* ===== Test 5: REDO + UNDO trong cùng log ===== */
static void test_mixed(void) {
    printf("\n[5] Mixed: tx1 commit (REDO), tx2 chưa commit (UNDO)\n");
    const char *db  = "phase4_mixed.hugo";
    const char *log = "phase4_mixed.hugolog";
    remove(db); remove(log);

    PageManager pm;
    pm_create(&pm, db);
    uint64_t p1, p2;
    pm_alloc_page(&pm, &p1);
    pm_alloc_page(&pm, &p2);
    HugoPage pg;
    memset(&pg, 0, sizeof(pg));
    pg.page_type = PAGE_TYPE_LEAF;

    pg.page_id = (uint32_t)p1;
    memset(pg.data, 0x11, HUGO_PAGE_DATA_SIZE);
    pm_write_page(&pm, &pg);

    pg.page_id = (uint32_t)p2;
    memset(pg.data, 0x22, HUGO_PAGE_DATA_SIZE);
    pm_write_page(&pm, &pg);
    pm_flush_header(&pm);

    Wal w;
    wal_open(&w, log);

    /* tx1: ghi page p1 offset 0..10 thành 0xFF, COMMIT, KHÔNG flush page */
    uint64_t tx1 = wal_new_tx_id(&w);
    wal_log_begin(&w, tx1);
    uint8_t b[10], a[10];
    memset(b, 0x11, 10); memset(a, 0xFF, 10);
    wal_log_update(&w, tx1, p1, 0, b, a, 10);
    wal_log_commit(&w, tx1);

    /* tx2: ghi page p2 offset 0..10 thành 0xEE, FLUSH page, KHÔNG commit */
    uint64_t tx2 = wal_new_tx_id(&w);
    wal_log_begin(&w, tx2);
    uint8_t b2[10], a2[10];
    memset(b2, 0x22, 10); memset(a2, 0xEE, 10);
    wal_log_update(&w, tx2, p2, 0, b2, a2, 10);
    wal_sync(&w);
    /* Flush page p2 (simulate eager write) */
    pg.page_id = (uint32_t)p2;
    memset(pg.data, 0x22, HUGO_PAGE_DATA_SIZE);
    memset(pg.data, 0xEE, 10);
    pm_write_page(&pm, &pg);
    /* CRASH */
    wal_close(&w);
    pm_close(&pm);

    /* Recovery */
    pm_open(&pm, db);
    wal_open(&w, log);
    wal_recover(&w, &pm);

    HugoPage c;
    pm_read_page(&pm, p1, &c);
    int p1_redone = 1;
    for (int i = 0; i < 10; i++) if (c.data[i] != 0xFF) { p1_redone = 0; break; }
    CHECK(p1_redone, "p1: tx1 committed → REDO applied (data = 0xFF)");

    pm_read_page(&pm, p2, &c);
    int p2_undone = 1;
    for (int i = 0; i < 10; i++) if (c.data[i] != 0x22) { p2_undone = 0; break; }
    CHECK(p2_undone, "p2: tx2 uncommitted → UNDO applied (data về 0x22)");

    wal_close(&w);
    pm_close(&pm);
    remove(db); remove(log);
}

/* ===== Test 6: Truncated tail (crash giữa append) ===== */
static void test_truncated(void) {
    printf("\n[6] Truncated tail: crash giữa write record\n");
    const char *log = "phase4_trunc.hugolog";
    remove(log);

    /* Ghi 3 record valid */
    Wal w;
    wal_open(&w, log);
    uint64_t tx = wal_new_tx_id(&w);
    wal_log_begin(&w, tx);
    uint8_t b[20], a[20]; memset(b, 1, 20); memset(a, 2, 20);
    wal_log_update(&w, tx, 1, 0, b, a, 20);
    wal_log_commit(&w, tx);
    wal_sync(&w);
    uint64_t valid_size = w.size;
    wal_close(&w);

    /* Append rác (simulate crash giữa lúc ghi record kế) */
    HugoFile *f = hugo_open(log, HUGO_OPEN_RDWR);
    uint8_t junk[15] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
                         9, 0, 0, 0, 0, 0, 0};
    hugo_write(f, junk, 15, valid_size);
    hugo_sync(f);
    hugo_close(f);

    /* Reopen → phải parse được 3 record valid, dừng ở record 4 truncated */
    Wal w2;
    wal_open(&w2, log);
    WalIter it;
    wal_iter_init(&it, &w2);
    WalRecord rec;
    int n = 0;
    while (wal_iter_next(&it, &rec) == WAL_OK) n++;
    CHECK(n == 3, "đọc được 3 record valid, dừng tại truncated");
    CHECK(w2.size == valid_size, "size set về vị trí record valid cuối");
    wal_close(&w2);
    remove(log);
}

int main(void) {
    printf("=== HUGO DB — Phase 4 (WAL + Crash Recovery) Tests ===\n");
    test_append_iterate();
    test_reopen();
    test_redo();
    test_undo();
    test_mixed();
    test_truncated();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
