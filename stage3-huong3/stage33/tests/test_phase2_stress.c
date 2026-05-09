/* test_phase2_stress.c — 1 triệu ops stress test
 * Tách riêng vì không kiểm invariant sau mỗi op (sẽ quá chậm).
 * Check invariant định kỳ + cuối + so với model.
 */
#include "../src/core/btree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 1000000;
    unsigned seed = (argc > 2) ? (unsigned)atoi(argv[2]) : 12345;
    printf("=== Stress: %d ops, seed=%u, BTREE_T=%d ===\n", N, seed, BTREE_T);

    BTree *t = btree_create();

    /* Model: bitmap của các key hiện tại + value array */
    int key_range = N;  /* keys trong [0, N) */
    uint8_t *present = calloc(key_range, 1);
    btree_val_t *vals = calloc(key_range, sizeof(btree_val_t));
    int model_count = 0;

    srand(seed);
    int divergence = 0, inv_fails = 0;
    int n_insert = 0, n_search = 0, n_delete = 0;
    int n_insert_ok = 0, n_delete_ok = 0;

    clock_t t0 = clock();

    for (int i = 0; i < N; i++) {
        int action = rand() % 100;
        btree_key_t k = (btree_key_t)(rand() % key_range);

        if (action < 50) {
            n_insert++;
            btree_val_t v = (btree_val_t)(k * 1000 + i);
            int br = btree_insert(t, k, v);
            int mr = present[k] ? -1 : 0;
            if (mr == 0) { present[k] = 1; vals[k] = v; model_count++; }
            if ((br == BT_OK) != (mr == 0)) divergence++;
            if (br == BT_OK) n_insert_ok++;
        } else if (action < 75) {
            n_search++;
            btree_val_t bv;
            int br = btree_search(t, k, &bv);
            if ((br == BT_OK) != (present[k] != 0)) divergence++;
            if (br == BT_OK && present[k] && bv != vals[k]) divergence++;
        } else {
            n_delete++;
            int br = btree_delete(t, k);
            int mr = present[k] ? 0 : -1;
            if (mr == 0) { present[k] = 0; model_count--; }
            if ((br == BT_OK) != (mr == 0)) divergence++;
            if (br == BT_OK) n_delete_ok++;
        }

        /* Check invariants định kỳ — mỗi 50k ops */
        if ((i + 1) % 50000 == 0) {
            if (btree_check_invariants(t) != 0) inv_fails++;
            if ((int)t->count != model_count) divergence++;
        }
    }

    clock_t t1 = clock();
    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;

    /* Final invariant + content check */
    int final_inv = btree_check_invariants(t);
    int find_fails = 0;
    for (int k = 0; k < key_range; k++) {
        if (!present[k]) continue;
        btree_val_t v;
        if (btree_search(t, k, &v) != BT_OK || v != vals[k]) find_fails++;
    }

    printf("Ops      : %d insert (%d ok), %d search, %d delete (%d ok)\n",
           n_insert, n_insert_ok, n_search, n_delete, n_delete_ok);
    printf("Time     : %.2fs (%.0f ops/s)\n", secs, N / (secs + 1e-9));
    printf("Tree size: %zu  (model: %d)\n", t->count, model_count);
    printf("Periodic invariant fails: %d\n", inv_fails);
    printf("Final invariant code    : %d (0 = OK)\n", final_inv);
    printf("Behavior divergences    : %d\n", divergence);
    printf("Final find mismatches   : %d\n", find_fails);

    int ok = (inv_fails == 0) && (final_inv == 0)
          && (divergence == 0) && (find_fails == 0)
          && ((int)t->count == model_count);

    btree_destroy(t);
    free(present); free(vals);

    printf("\n=== %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
