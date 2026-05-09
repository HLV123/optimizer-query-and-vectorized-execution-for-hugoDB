/* test_phase5.c — Lock Manager + 2PL tests
 *
 * Phase 5 acceptance:
 *   [x] Conflict matrix S/S, S/X, X/S, X/X
 *   [x] Multiple readers cùng giữ S
 *   [x] Lock upgrade S → X
 *   [x] FIFO fairness (waiter đứng trước phải được grant trước)
 *   [x] Deadlock detection (wait-for cycle)
 *   [x] 2PL phase: sau release không acquire được nữa
 *   [x] Release_all + grant waiters tiếp
 */
#include "../src/core/lock_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* ===== Test 1: conflict matrix ===== */
static void test_conflict_matrix(void) {
    printf("\n[1] Conflict matrix S/S, S/X, X/S, X/X\n");

    /* S/S compatible */
    {
        LockManager lm; lm_init(&lm);
        CHECK(lm_acquire(&lm, 1, 100, LM_S) == LM_OK, "tx1 S");
        CHECK(lm_acquire(&lm, 2, 100, LM_S) == LM_OK, "tx2 S đồng thời (S/S OK)");
        lm_destroy(&lm);
    }
    /* S/X conflict */
    {
        LockManager lm; lm_init(&lm);
        lm_acquire(&lm, 1, 100, LM_S);
        CHECK(lm_acquire(&lm, 2, 100, LM_X) == LM_WAIT, "tx2 X conflict S → wait");
        lm_destroy(&lm);
    }
    /* X/S conflict */
    {
        LockManager lm; lm_init(&lm);
        lm_acquire(&lm, 1, 100, LM_X);
        CHECK(lm_acquire(&lm, 2, 100, LM_S) == LM_WAIT, "tx2 S conflict X → wait");
        lm_destroy(&lm);
    }
    /* X/X conflict */
    {
        LockManager lm; lm_init(&lm);
        lm_acquire(&lm, 1, 100, LM_X);
        CHECK(lm_acquire(&lm, 2, 100, LM_X) == LM_WAIT, "tx2 X conflict X → wait");
        lm_destroy(&lm);
    }
    /* Khác resource → independent */
    {
        LockManager lm; lm_init(&lm);
        lm_acquire(&lm, 1, 100, LM_X);
        CHECK(lm_acquire(&lm, 2, 200, LM_X) == LM_OK, "khác resource → OK");
        lm_destroy(&lm);
    }
}

/* ===== Test 2: multiple readers ===== */
static void test_multiple_readers(void) {
    printf("\n[2] Nhiều tx cùng giữ S\n");
    LockManager lm; lm_init(&lm);
    for (int i = 1; i <= 10; i++) {
        int rc = lm_acquire(&lm, i, 500, LM_S);
        if (rc != LM_OK) { printf("    tx %d acquire S failed\n", i); }
    }
    int all_hold = 1;
    for (int i = 1; i <= 10; i++) if (!lm_holds(&lm, i, 500, LM_S)) all_hold = 0;
    CHECK(all_hold, "10 tx cùng giữ S thành công");
    lm_destroy(&lm);
}

/* ===== Test 3: re-acquire idempotent ===== */
static void test_reacquire(void) {
    printf("\n[3] Re-acquire cùng tx + cùng mode → OK (idempotent)\n");
    LockManager lm; lm_init(&lm);
    CHECK(lm_acquire(&lm, 1, 100, LM_S) == LM_OK, "first S");
    CHECK(lm_acquire(&lm, 1, 100, LM_S) == LM_OK, "second S OK");
    CHECK(lm_acquire(&lm, 1, 100, LM_X) == LM_OK, "upgrade S→X (no other holder)");
    CHECK(lm_holds(&lm, 1, 100, LM_X), "tx1 giữ X");
    lm_destroy(&lm);
}

/* ===== Test 4: lock upgrade với conflict ===== */
static void test_upgrade_blocked(void) {
    printf("\n[4] Upgrade S→X bị block khi có S khác\n");
    LockManager lm; lm_init(&lm);
    lm_acquire(&lm, 1, 100, LM_S);
    lm_acquire(&lm, 2, 100, LM_S);
    /* tx1 muốn upgrade S→X nhưng tx2 còn giữ S → wait */
    int rc = lm_acquire(&lm, 1, 100, LM_X);
    CHECK(rc == LM_WAIT, "upgrade khi có S khác → wait");
    /* tx2 release → tx1 upgrade thành công (granted khi try_grant_waiters) */
    lm_release(&lm, 2, 100);
    CHECK(lm_holds(&lm, 1, 100, LM_X), "sau tx2 release: tx1 đã upgrade thành X");
    lm_destroy(&lm);
}

/* ===== Test 5: FIFO fairness ===== */
static void test_fifo_fairness(void) {
    printf("\n[5] FIFO fairness — waiter X đứng trước block S đến sau\n");
    LockManager lm; lm_init(&lm);
    lm_acquire(&lm, 1, 100, LM_S);                        /* tx1 holds S */
    int rc2 = lm_acquire(&lm, 2, 100, LM_X);              /* tx2 wait X */
    CHECK(rc2 == LM_WAIT, "tx2 X wait");
    int rc3 = lm_acquire(&lm, 3, 100, LM_S);              /* tx3 cũng phải wait */
    CHECK(rc3 == LM_WAIT, "tx3 S wait (vì tx2 X đứng trước queue)");
    /* tx1 release → grant tx2 (X), tx3 vẫn wait */
    lm_release(&lm, 1, 100);
    CHECK(lm_holds(&lm, 2, 100, LM_X), "tx2 X granted");
    CHECK(!lm_holds(&lm, 3, 100, LM_S), "tx3 vẫn chưa được grant");
    /* tx2 release → grant tx3 */
    lm_release(&lm, 2, 100);
    CHECK(lm_holds(&lm, 3, 100, LM_S), "tx3 granted sau khi tx2 release");
    lm_destroy(&lm);
}

/* ===== Test 6: deadlock detection ===== */
static void test_deadlock_simple(void) {
    printf("\n[6] Deadlock detection: 2 tx, 2 resource\n");
    LockManager lm; lm_init(&lm);
    /* Classic: tx1 X(A), tx2 X(B), tx1 muốn X(B), tx2 muốn X(A) */
    lm_acquire(&lm, 1, 100, LM_X);     /* tx1 X(100) */
    lm_acquire(&lm, 2, 200, LM_X);     /* tx2 X(200) */
    int rc1 = lm_acquire(&lm, 1, 200, LM_X);  /* tx1 wait tx2 */
    CHECK(rc1 == LM_WAIT, "tx1 wait tx2 trên 200");
    int rc2 = lm_acquire(&lm, 2, 100, LM_X);  /* tx2 wait tx1 → cycle */
    CHECK(rc2 == LM_DEADLOCK, "tx2 wait tx1 trên 100 → DEADLOCK");
    /* Abort tx2 → release_all → tx1 được grant */
    lm_release_all(&lm, 2);
    CHECK(lm_holds(&lm, 1, 200, LM_X), "sau abort tx2: tx1 lấy được 200");
    lm_destroy(&lm);
}

static void test_deadlock_3way(void) {
    printf("\n[7] Deadlock detection: 3-way cycle\n");
    LockManager lm; lm_init(&lm);
    /* tx1 X(A), tx2 X(B), tx3 X(C); tx1 wait B, tx2 wait C, tx3 wait A */
    lm_acquire(&lm, 1, 100, LM_X);
    lm_acquire(&lm, 2, 200, LM_X);
    lm_acquire(&lm, 3, 300, LM_X);
    CHECK(lm_acquire(&lm, 1, 200, LM_X) == LM_WAIT, "tx1 wait tx2");
    CHECK(lm_acquire(&lm, 2, 300, LM_X) == LM_WAIT, "tx2 wait tx3");
    int rc3 = lm_acquire(&lm, 3, 100, LM_X);
    CHECK(rc3 == LM_DEADLOCK, "tx3 wait tx1 → 3-cycle DEADLOCK");
    lm_destroy(&lm);
}

static void test_no_false_deadlock(void) {
    printf("\n[8] KHÔNG báo deadlock khi chỉ wait đơn giản\n");
    LockManager lm; lm_init(&lm);
    lm_acquire(&lm, 1, 100, LM_X);
    int rc = lm_acquire(&lm, 2, 100, LM_X);
    CHECK(rc == LM_WAIT, "tx2 wait tx1 (no cycle) — không phải deadlock");
    lm_destroy(&lm);
}

/* ===== Test 9: 2PL phase rule ===== */
static void test_2pl_phase(void) {
    printf("\n[9] 2PL phase: sau release không acquire thêm\n");
    LockManager lm; lm_init(&lm);
    lm_acquire(&lm, 1, 100, LM_S);
    lm_acquire(&lm, 1, 200, LM_S);
    lm_release(&lm, 1, 100);  /* shrinking phase bắt đầu */
    int rc = lm_acquire(&lm, 1, 300, LM_S);
    CHECK(rc == LM_PHASE_VIOLATION, "acquire sau release → PHASE_VIOLATION");
    lm_destroy(&lm);
}

/* ===== Test 10: release_all + grant chain ===== */
static void test_release_all_grants(void) {
    printf("\n[10] release_all → các waiter được grant\n");
    LockManager lm; lm_init(&lm);
    lm_acquire(&lm, 1, 100, LM_X);
    lm_acquire(&lm, 2, 100, LM_S);  /* wait */
    lm_acquire(&lm, 3, 100, LM_S);  /* wait */

    int n = lm_release_all(&lm, 1);
    CHECK(n == 1, "tx1 release 1 lock");
    /* tx2 + tx3 đều S → cả 2 phải được grant */
    CHECK(lm_holds(&lm, 2, 100, LM_S), "tx2 granted S");
    CHECK(lm_holds(&lm, 3, 100, LM_S), "tx3 granted S");
    lm_destroy(&lm);
}

/* ===== Test 11: stress — random concurrent-style ops, single thread ===== */
static void test_stress(void) {
    printf("\n[11] Stress: 1000 random ops, kiểm tra invariant\n");
    LockManager lm; lm_init(&lm);

    enum { N_TX = 5, N_RES = 10 };
    /* Mỗi tx track lock state mình giữ → verify với lm */
    int holds[N_TX + 1][N_RES + 1];  /* 0=none, 1=S, 2=X */
    memset(holds, 0, sizeof(holds));
    int shrinking[N_TX + 1] = {0};

    srand(42);
    int n_acq = 0, n_rel = 0, n_abort = 0;
    int n_inv_fail = 0;

    for (int op = 0; op < 1000; op++) {
        int tx = 1 + rand() % N_TX;
        int res = 1 + rand() % N_RES;
        int action = rand() % 100;

        if (action < 50) {
            /* acquire random mode */
            int mode = (rand() % 2);
            int rc = lm_acquire(&lm, tx, res, mode);
            if (rc == LM_OK) {
                int cur = holds[tx][res];
                int new_mode = (mode == LM_X) ? 2 : (cur >= 1 ? cur : 1);
                holds[tx][res] = new_mode;
                n_acq++;
            } else if (rc == LM_DEADLOCK || rc == LM_WAIT) {
                /* Single-thread: can't actually wait. Treat WAIT cũng như abort
                 * (vì waiter sẽ được grant ngầm khi tx khác release → model lệch). */
                lm_release_all(&lm, tx);
                memset(holds[tx], 0, sizeof(holds[tx]));
                shrinking[tx] = 0;
                if (rc == LM_DEADLOCK) n_abort++;
            } else if (rc == LM_PHASE_VIOLATION) {
                /* Tx đang trong shrinking phase → reset bằng release_all */
                lm_release_all(&lm, tx);
                memset(holds[tx], 0, sizeof(holds[tx]));
                shrinking[tx] = 0;
            }
        } else if (action < 75) {
            if (holds[tx][res] != 0) {
                lm_release(&lm, tx, res);
                holds[tx][res] = 0;
                shrinking[tx] = 1;
                n_rel++;
            }
        } else {
            /* release_all = commit/abort */
            lm_release_all(&lm, tx);
            memset(holds[tx], 0, sizeof(holds[tx]));
            shrinking[tx] = 0;
        }

        /* Invariant: với mỗi (tx, res), lm_holds khớp với model */
        for (int t = 1; t <= N_TX; t++) {
            for (int r = 1; r <= N_RES; r++) {
                int expect_s = holds[t][r] >= 1;
                int expect_x = holds[t][r] == 2;
                int actual_s = lm_holds(&lm, t, r, LM_S);
                int actual_x = lm_holds(&lm, t, r, LM_X);
                if (expect_s != actual_s || expect_x != actual_x) {
                    n_inv_fail++;
                }
            }
        }
    }
    printf("       %d acquires OK, %d releases, %d aborted (deadlock)\n",
           n_acq, n_rel, n_abort);
    CHECK(n_inv_fail == 0, "invariant tx state khớp lm trong 1000 ops");
    lm_destroy(&lm);
}

int main(void) {
    printf("=== HUGO DB — Phase 5 (Lock Manager + 2PL) Tests ===\n");
    test_conflict_matrix();
    test_multiple_readers();
    test_reacquire();
    test_upgrade_blocked();
    test_fifo_fairness();
    test_deadlock_simple();
    test_deadlock_3way();
    test_no_false_deadlock();
    test_2pl_phase();
    test_release_all_grants();
    test_stress();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
