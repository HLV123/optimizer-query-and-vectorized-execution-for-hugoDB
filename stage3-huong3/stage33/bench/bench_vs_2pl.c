/* bench_vs_2pl.c — So sánh LSM engine vs 2PL (B-tree) engine
 *
 * Chạy cùng 5 workloads W1-W5 trên cả hai engine:
 *   W1_reads       — 100% read (random get)
 *   W2_hotwrite    — 100% write vào 1 key nóng
 *   W3_multiwrite  — 100% write random keys
 *   W4_mixed       — 70% read + 30% write
 *   W5_longreader  — sequential scan toàn bộ
 *
 * Build:
 *   Windows:
 *     gcc -Wall -O2 -std=c11 -Isrc/core -Isrc/query bench\bench_vs_2pl.c
 *       src\core\checksum.c src\core\hugo_io_win.c src\core\page.c
 *       src\core\btree.c src\core\dbtree.c src\core\buffer_pool.c
 *       src\core\wal.c src\core\lock_manager.c src\core\collection.c
 *       src\core\disk_db.c src\core\executor_disk.c
 *       src\query\tokenizer.c src\query\parser.c src\query\executor.c
 *       src\core\lsm\arena.c src\core\lsm\memtable.c src\core\lsm\bloom.c
 *       src\core\lsm\lsm_wal.c src\core\lsm\sstable_builder.c
 *       src\core\lsm\sstable_reader.c src\core\lsm\manifest.c
 *       src\core\lsm\lsm.c -o bench_vs_2pl.exe -lm
 *
 *   Linux:
 *     gcc -Wall -O2 -std=c11 -Isrc/core -Isrc/query bench/bench_vs_2pl.c
 *       [same sources] -o bench_vs_2pl -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../src/core/disk_db.h"
#include "../src/core/lsm/lsm.h"

/* ---- Timer ---- */
#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

/* ---- Config ---- */
#define N_SEED    1000    /* số keys seed trước khi bench */
#define N_OPS     5000    /* số ops mỗi workload */
#define VAL_SIZE  64      /* bytes per value */
#define HOT_KEY   "hotkey_000000"

/* ---- RNG đơn giản ---- */
static unsigned g_seed = 12345;
static unsigned rng(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

/* ---- Latency tracking ---- */
#define MAX_SAMPLES 10000
static double g_lat[MAX_SAMPLES];
static int    g_lat_n;

static void lat_reset(void) { g_lat_n = 0; }
static void lat_add(double v) { if (g_lat_n < MAX_SAMPLES) g_lat[g_lat_n++] = v; }

static int cmp_double(const void *a, const void *b) {
    double x = *(double*)a, y = *(double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static double lat_pct(double p) {
    if (g_lat_n == 0) return 0;
    qsort(g_lat, g_lat_n, sizeof(double), cmp_double);
    int idx = (int)(p * g_lat_n / 100.0);
    if (idx >= g_lat_n) idx = g_lat_n - 1;
    return g_lat[idx] * 1e6; /* microseconds */
}

/* ================================================================
 * 2PL (DiskDB) helpers
 * ================================================================ */

static DiskDB   g_2pl_db;
static DiskColl *g_2pl_coll;

static void twopl_setup(const char *path) {
#ifdef _WIN32
    char cmd[300]; snprintf(cmd, sizeof(cmd), "rmdir /s /q %s 2>nul", path); system(cmd);
    snprintf(cmd, sizeof(cmd), "del /q %s.hugo 2>nul", path); system(cmd);
#else
    char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s %s.hugo", path, path); system(cmd);
#endif
    char dbpath[300]; snprintf(dbpath, sizeof(dbpath), "%s.hugo", path);
    ddb_create(&g_2pl_db, "bench", dbpath);
    g_2pl_coll = ddb_create_coll(&g_2pl_db, "kv");
    assert(g_2pl_coll);

    /* seed N_SEED docs */
    char val[VAL_SIZE]; memset(val, 'x', sizeof(val));
    for (int i = 0; i < N_SEED; i++) {
        char key[24]; snprintf(key, 24, "key%09d", i);
        Document doc = {0};
        KVPair kv = {0};
        snprintf(kv.key, sizeof(kv.key), "k");
        kv.value.type = VAL_STR;
        memcpy(kv.value.str, key, strlen(key));
        doc.pairs = &kv; doc.count = 1;
        uint64_t id;
        ddb_insert_doc(&g_2pl_db, g_2pl_coll, &doc, &id);
    }
}

static void twopl_teardown(void) { ddb_close(&g_2pl_db); }

static int twopl_write(const char *key, const char *val) {
    Document doc = {0};
    KVPair kv = {0};
    snprintf(kv.key, sizeof(kv.key), "k");
    kv.value.type = VAL_STR;
    snprintf(kv.value.str, sizeof(kv.value.str), "%s", val);
    doc.pairs = &kv; doc.count = 1;
    uint64_t id;
    (void)key;
    return ddb_insert_doc(&g_2pl_db, g_2pl_coll, &doc, &id);
}

typedef struct { int count; } ScanCtx2PL;
static void scan_cb_2pl(uint64_t id, Document *doc, void *ctx) {
    (void)id; (void)doc;
    ((ScanCtx2PL*)ctx)->count++;
}

/* ================================================================
 * LSM helpers
 * ================================================================ */

static Lsm *g_lsm;

static void lsm_setup(const char *path) {
#ifdef _WIN32
    char cmd[300]; snprintf(cmd, sizeof(cmd), "rmdir /s /q %s 2>nul", path); system(cmd);
#else
    char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", path); system(cmd);
#endif
    g_lsm = lsm_open(path);
    assert(g_lsm);

    /* seed N_SEED keys */
    char val[VAL_SIZE]; memset(val, 'x', sizeof(val));
    for (int i = 0; i < N_SEED; i++) {
        char key[24]; snprintf(key, 24, "key%09d", i);
        lsm_put(g_lsm, key, strlen(key), val, sizeof(val));
    }
}

static void lsm_teardown(void) { lsm_close(g_lsm); g_lsm = NULL; }

typedef struct { int count; } ScanCtxLsm;
static int scan_cb_lsm(const void *key, size_t kl,
                        const void *val, size_t vl,
                        uint64_t seq, uint8_t op, void *ctx) {
    (void)key;(void)kl;(void)val;(void)vl;(void)seq;(void)op;
    ((ScanCtxLsm*)ctx)->count++;
    return LSM_OK;
}

/* ================================================================
 * Workload runners
 * ================================================================ */

typedef enum { ENGINE_2PL, ENGINE_LSM } Engine;

static double run_workload(Engine eng, int workload) {
    char val[VAL_SIZE]; memset(val, 'v', sizeof(val)); val[VAL_SIZE-1] = '\0';
    lat_reset();
    g_seed = 42;

    double t0 = now_sec();

    for (int i = 0; i < N_OPS; i++) {
        double op_t0 = now_sec();
        unsigned r = rng() % N_SEED;
        char key[24]; snprintf(key, 24, "key%09u", r);

        switch (workload) {
        case 1: { /* W1: 100% read */
            if (eng == ENGINE_LSM) {
                void *v; size_t vl;
                int ret = lsm_get(g_lsm, key, strlen(key), &v, &vl);
                if (ret == LSM_OK) free(v);
            } else {
                /* 2PL: scan to find by key value */
                ScanCtx2PL sc = {0};
                ddb_scan(&g_2pl_db, g_2pl_coll, scan_cb_2pl, &sc);
            }
            break;
        }
        case 2: { /* W2: 100% hot write */
            if (eng == ENGINE_LSM)
                lsm_put(g_lsm, HOT_KEY, strlen(HOT_KEY), val, VAL_SIZE);
            else
                twopl_write(HOT_KEY, val);
            break;
        }
        case 3: { /* W3: 100% random write */
            if (eng == ENGINE_LSM)
                lsm_put(g_lsm, key, strlen(key), val, VAL_SIZE);
            else
                twopl_write(key, val);
            break;
        }
        case 4: { /* W4: 70% read + 30% write */
            int is_read = (rng() % 10) < 7;
            if (is_read) {
                if (eng == ENGINE_LSM) {
                    void *v; size_t vl;
                    int ret = lsm_get(g_lsm, key, strlen(key), &v, &vl);
                    if (ret == LSM_OK) free(v);
                } else {
                    ScanCtx2PL sc = {0};
                    ddb_scan(&g_2pl_db, g_2pl_coll, scan_cb_2pl, &sc);
                }
            } else {
                if (eng == ENGINE_LSM)
                    lsm_put(g_lsm, key, strlen(key), val, VAL_SIZE);
                else
                    twopl_write(key, val);
            }
            break;
        }
        case 5: { /* W5: sequential scan */
            if (eng == ENGINE_LSM) {
                ScanCtxLsm sc = {0};
                lsm_scan(g_lsm, scan_cb_lsm, &sc);
            } else {
                ScanCtx2PL sc = {0};
                ddb_scan(&g_2pl_db, g_2pl_coll, scan_cb_2pl, &sc);
            }
            break;
        }
        }

        lat_add(now_sec() - op_t0);
    }

    return now_sec() - t0;
}

/* ================================================================
 * Main
 * ================================================================ */

static const char *wname[] = {
    "", "W1_reads", "W2_hotwrite", "W3_multiwrite", "W4_mixed", "W5_longreader"
};

int main(void) {
    printf("Seeding %d docs, running %d ops per workload...\n\n", N_SEED, N_OPS);
    printf("%-6s %-15s %10s %10s %8s %8s\n",
           "Mode", "Workload", "ops/sec", "elapsed", "p50µs", "p99µs");
    printf("%-6s %-15s %10s %10s %8s %8s\n",
           "------", "---------------", "--------", "----------", "-------", "-------");

    for (int w = 1; w <= 5; w++) {
        /* ---- 2PL ---- */
        twopl_setup("bench_2pl_data");
        double elapsed = run_workload(ENGINE_2PL, w);
        double ops = N_OPS / elapsed;
        double p50 = lat_pct(50), p99 = lat_pct(99);
        printf("%-6s %-15s %10.0f %9.3fs %8.1f %8.1f\n",
               "2PL", wname[w], ops, elapsed, p50, p99);
        twopl_teardown();

        /* ---- LSM ---- */
        lsm_setup("bench_lsm_data");
        elapsed = run_workload(ENGINE_LSM, w);
        ops = N_OPS / elapsed;
        p50 = lat_pct(50); p99 = lat_pct(99);
        printf("%-6s %-15s %10.0f %9.3fs %8.1f %8.1f\n\n",
               "LSM", wname[w], ops, elapsed, p50, p99);
        lsm_teardown();
    }

    printf("Done.\n");
    return 0;
}

