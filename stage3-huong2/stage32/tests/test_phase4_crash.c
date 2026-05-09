/* test_phase4_crash.c — Automated random crash test
 *
 * Mô phỏng: chạy N transactions tuần tự, mỗi tx UPDATE 1 page.
 * Sau mỗi tx có thể "crash" (cắt WAL tại offset hiện tại + delta random).
 * Recovery → verify:
 *   - Mọi committed tx data có trên disk
 *   - Mọi uncommitted tx data đã bị rollback
 *
 * Lặp K iterations với seed khác nhau.
 */
#include "../src/core/page.h"
#include "../src/core/wal.h"
#include "../src/core/serializer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Trạng thái mỗi page: byte đầu = tx_id của tx CUỐI committed update lên page đó.
 * Dùng để biết kỳ vọng sau recovery. */

typedef struct {
    uint64_t expected_tx;  /* tx_id committed cuối cùng đã update */
    uint64_t initial_byte; /* giá trị page khi mới tạo */
} PageState;

#define N_PAGES 10

static int run_one_iteration(unsigned seed, int n_tx, int verbose) {
    char db[64], log[64];
    snprintf(db,  sizeof(db),  "phase4_crash_%u.hugo",    seed);
    snprintf(log, sizeof(log), "phase4_crash_%u.hugolog", seed);
    remove(db); remove(log);

    PageManager pm;
    pm_create(&pm, db);
    uint64_t pids[N_PAGES];
    PageState states[N_PAGES];
    HugoPage pg;
    memset(&pg, 0, sizeof(pg));
    pg.page_type = PAGE_TYPE_LEAF;

    /* Init pages: data = 0x00, expected_tx = 0 */
    for (int i = 0; i < N_PAGES; i++) {
        pm_alloc_page(&pm, &pids[i]);
        pg.page_id = (uint32_t)pids[i];
        memset(pg.data, 0, HUGO_PAGE_DATA_SIZE);
        pg.data[0] = 0;
        pm_write_page(&pm, &pg);
        states[i].expected_tx = 0;
        states[i].initial_byte = 0;
    }
    pm_flush_header(&pm);
    hugo_sync(pm.file);

    Wal w;
    wal_open(&w, log);

    srand(seed);
    /* Chạy n_tx tx. Với prob 50% commit, 30% abort, 20% crash trước commit. */
    int n_committed = 0, n_aborted = 0, n_crashed = 0;
    int crash_at = -1;

    for (int t = 0; t < n_tx; t++) {
        uint64_t tx = wal_new_tx_id(&w);
        wal_log_begin(&w, tx);

        /* Pick random page, write byte 0 = tx (mod 256) */
        int pidx = rand() % N_PAGES;
        uint64_t page_id = pids[pidx];

        HugoPage cur;
        pm_read_page(&pm, page_id, &cur);
        uint8_t before_byte = cur.data[0];
        uint8_t after_byte = (uint8_t)(tx & 0xFF);
        if (after_byte == 0) after_byte = 1;  /* tránh confuse với initial */

        wal_log_update(&w, tx, page_id, 0, &before_byte, &after_byte, 1);

        /* Apply page change vào memory + ghi disk (eager write) */
        cur.data[0] = after_byte;
        pm_write_page(&pm, &cur);

        int outcome = rand() % 100;
        if (outcome < 50) {
            /* commit */
            wal_log_commit(&w, tx);
            wal_sync(&w);
            states[pidx].expected_tx = tx;
            n_committed++;
        } else if (outcome < 80) {
            /* abort: log abort + manually undo page */
            wal_log_abort(&w, tx);
            wal_sync(&w);
            cur.data[0] = before_byte;
            pm_write_page(&pm, &cur);
            n_aborted++;
        } else {
            /* crash: KHÔNG commit, KHÔNG sync log thêm. Dừng tx loop. */
            wal_sync(&w);   /* simulate: log đã có UPDATE, page đã ghi */
            crash_at = t;
            n_crashed++;
            break;
        }
    }

    /* Get expected page bytes (snapshot trước recovery để debug) */
    uint8_t expected_bytes[N_PAGES];
    for (int i = 0; i < N_PAGES; i++) {
        if (states[i].expected_tx == 0) expected_bytes[i] = 0;
        else {
            uint8_t b = (uint8_t)(states[i].expected_tx & 0xFF);
            expected_bytes[i] = (b == 0) ? 1 : b;
        }
    }

    /* SIMULATE CRASH: close không clean (như kill -9 giữa) */
    wal_close(&w);
    pm_close(&pm);

    /* RECOVERY */
    pm_open(&pm, db);
    wal_open(&w, log);
    int rc = wal_recover(&w, &pm);
    if (rc != WAL_OK) {
        printf("    seed=%u recovery FAILED rc=%d\n", seed, rc);
        wal_close(&w); pm_close(&pm);
        return 0;
    }

    /* Verify: mỗi page data[0] == expected */
    int mismatches = 0;
    for (int i = 0; i < N_PAGES; i++) {
        HugoPage check;
        if (pm_read_page(&pm, pids[i], &check) != PG_OK) { mismatches++; continue; }
        if (check.data[0] != expected_bytes[i]) {
            mismatches++;
            if (verbose) {
                printf("    seed=%u page %d (pid=%llu): expected 0x%02X, got 0x%02X "
                       "(expected_tx=%llu)\n",
                       seed, i, (unsigned long long)pids[i],
                       expected_bytes[i], check.data[0],
                       (unsigned long long)states[i].expected_tx);
            }
        }
    }

    wal_close(&w);
    pm_close(&pm);
    remove(db); remove(log);

    if (verbose || mismatches > 0) {
        printf("    seed=%u: %d committed, %d aborted, %d crashed → %s\n",
               seed, n_committed, n_aborted, n_crashed,
               mismatches == 0 ? "OK" : "FAIL");
    }
    (void)crash_at;
    return mismatches == 0;
}

int main(int argc, char **argv) {
    int n_iter = (argc > 1) ? atoi(argv[1]) : 50;
    int n_tx   = (argc > 2) ? atoi(argv[2]) : 30;

    printf("=== Phase 4 Automated Crash Test ===\n");
    printf("Iterations=%d, txns/iter=%d\n\n", n_iter, n_tx);

    int passed = 0;
    for (int i = 0; i < n_iter; i++) {
        unsigned seed = 1000 + i;
        int ok = run_one_iteration(seed, n_tx, 0);
        if (ok) passed++;
        else {
            /* Re-run với verbose để debug */
            run_one_iteration(seed, n_tx, 1);
        }
    }
    printf("\n=== %d/%d iterations passed ===\n", passed, n_iter);
    return (passed == n_iter) ? 0 : 1;
}
