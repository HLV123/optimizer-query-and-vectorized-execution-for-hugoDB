/* bench/bench_mvcc.c — 2PL vs MVCC Benchmark (Phase 7)
 *
 * Compile (Linux/macOS):
 *   gcc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I src/core -I src/query \
 *       bench/bench_mvcc.c \
 *       src/core/checksum.c src/core/hugo_io_posix.c src/core/page.c \
 *       src/core/wal.c src/core/disk_db.c src/core/collection.c \
 *       src/core/executor_disk.c \
 *       src/core/ts_oracle.c src/core/mvcc_tx.c \
 *       src/core/doc_version.c src/core/mvcc_read.c src/core/mvcc_write.c \
 *       src/core/mvcc_recovery.c src/core/mvcc_vacuum.c \
 *       src/query/tokenizer.c src/query/parser.c src/query/executor.c \
 *       -o bench_mvcc
 *
 * Usage:
 *   ./bench_mvcc [ops] [docs]
 *   Default: 5000 ops, 1000 docs seed
 *
 * Workloads:
 *   W1: 100% reads  (should be similar between 2PL/MVCC)
 *   W2: 100% writes, single hot key
 *   W3: 100% writes, many keys (MVCC slight overhead per write)
 *   W4: Mixed 50/50 reads+writes
 *   W5: Long-running reader + concurrent writers
 *
 * Output: CSV to stdout + summary to stderr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "disk_db.h"
#include "mvcc_tx.h"
#include "mvcc_read.h"
#include "mvcc_write.h"
#include "mvcc_vacuum.h"
#include "collection.h"
#include "../query/ast.h"

/* ===== Timing - cross-platform ===== */
#ifdef _WIN32
#  include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
#  include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

/* ===== Config ===== */
#define BENCH_DB_2PL   "bench_2pl.hugo"
#define BENCH_DB_MVCC  "bench_mvcc.hugo"
#define BENCH_COLL     "bench"

static int  g_ops  = 5000;
static int  g_docs = 1000;

/* ===== Latency tracking ===== */
#define MAX_SAMPLES 100000

typedef struct {
    double *samples;
    int     n;
    int     capacity;
} LatTracker;

static void lat_init(LatTracker *t) {
    t->capacity = MAX_SAMPLES;
    t->samples  = (double*)malloc(t->capacity * sizeof(double));
    t->n        = 0;
}
static void lat_record(LatTracker *t, double secs) {
    if (t->n < t->capacity) t->samples[t->n++] = secs * 1e6;  /* µs */
}
static int cmp_double(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}
static double lat_percentile(LatTracker *t, double pct) {
    if (t->n == 0) return 0;
    qsort(t->samples, t->n, sizeof(double), cmp_double);
    int idx = (int)(pct * t->n / 100.0);
    if (idx >= t->n) idx = t->n - 1;
    return t->samples[idx];
}
static void lat_free(LatTracker *t) { free(t->samples); }

/* ===== DB helpers ===== */

static Document* make_bench_doc(int id, int value) {
    Document *d = (Document*)calloc(1, sizeof(Document));
    /* field: id */
    KVPair *kv1 = (KVPair*)calloc(1, sizeof(KVPair));
    strcpy(kv1->key, "id");
    kv1->value.type = VAL_NUM;
    kv1->value.num  = (double)id;
    /* field: value */
    KVPair *kv2 = (KVPair*)calloc(1, sizeof(KVPair));
    strcpy(kv2->key, "value");
    kv2->value.type = VAL_NUM;
    kv2->value.num  = (double)value;
    kv1->next = kv2;
    d->pairs  = kv1;
    d->count  = 2;
    return d;
}

static void seed_db_2pl(DiskDB *db) {
    DiskColl *c = ddb_create_coll(db, BENCH_COLL);
    for (int i = 0; i < g_docs; i++) {
        Document *d = make_bench_doc(i + 1, i * 10);
        uint64_t id;
        ddb_insert_doc(db, c, d, &id);
        doc_free(d);
    }
}

static void seed_db_mvcc(DiskDB *db) {
    ddb_create_coll(db, BENCH_COLL);
    MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                 ts_oracle_next(&db->mvcc_oracle),
                                 NULL, 0);
    mvcc_registry_add(&db->mvcc_registry, tx);
    for (int i = 0; i < g_docs; i++) {
        Document *d = make_bench_doc(i + 1, i * 10);
        uint64_t id;
        mvcc_insert_doc(db, tx, BENCH_COLL, d, &id);
        doc_free(d);
    }
    mvcc_commit_tx(db, tx);
    mvcc_tx_free(tx);
}

/* ===== Result struct ===== */
typedef struct {
    const char *mode;
    const char *workload;
    int    ops;
    double elapsed_sec;
    double throughput;    /* ops/sec */
    double p50_us;
    double p99_us;
    int    aborts;
} BenchResult;

static void print_result(FILE *f, const BenchResult *r) {
    fprintf(f, "%s,%s,%d,%.3f,%.0f,%.1f,%.1f,%d\n",
            r->mode, r->workload, r->ops,
            r->elapsed_sec, r->throughput,
            r->p50_us, r->p99_us, r->aborts);
}

/* ===================================================================
 * Workload W1: 100% reads
 * =================================================================== */

static BenchResult bench_w1_2pl(DiskDB *db) {
    DiskColl *c = ddb_get_coll(db, BENCH_COLL);
    LatTracker lat; lat_init(&lat);
    int aborts = 0;
    double t0 = now_sec();

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();

        /* 2PL: direct read, no lock (read-only, single-thread) */
        Document *d = ddb_read_doc(db, c, doc_id);
        if (d) doc_free(d);

        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "2PL", "W1_reads", g_ops, elapsed,
                      g_ops / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99),
                      aborts };
    lat_free(&lat);
    return r;
}

static BenchResult bench_w1_mvcc(DiskDB *db) {
    LatTracker lat; lat_init(&lat);
    int aborts = 0;
    double t0 = now_sec();

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();

        MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                     ts_oracle_next(&db->mvcc_oracle),
                                     NULL, 0);
        mvcc_registry_add(&db->mvcc_registry, tx);
        int err;
        Document *d = mvcc_find_doc(db, tx, BENCH_COLL, doc_id, &err);
        if (d) doc_free(d);
        mvcc_commit_tx(db, tx);
        mvcc_tx_free(tx);

        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "MVCC", "W1_reads", g_ops, elapsed,
                      g_ops / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99),
                      aborts };
    lat_free(&lat);
    return r;
}

/* ===================================================================
 * Workload W2: 100% writes, single hot key
 * =================================================================== */

static BenchResult bench_w2_2pl(DiskDB *db) {
    DiskColl *c = ddb_get_coll(db, BENCH_COLL);
    LatTracker lat; lat_init(&lat);
    double t0 = now_sec();
    int ops_done = 0;

    for (int i = 0; i < g_ops; i++) {
        double s = now_sec();
        Document *d = make_bench_doc(1, i);  /* hot key = doc 1 */
        uint64_t tx = wal_new_tx_id(&db->wal);
        wal_log_begin(&db->wal, tx);
        int rc = ddb_update_doc(db, c, 1, d);
        doc_free(d);
        if (rc == 0) {
            wal_log_commit(&db->wal, tx);
            ops_done++;
        } else {
            wal_log_abort(&db->wal, tx);
        }
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "2PL", "W2_hotwrite", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), 0 };
    lat_free(&lat);
    return r;
}

static BenchResult bench_w2_mvcc(DiskDB *db) {
    LatTracker lat; lat_init(&lat);
    int aborts = 0;
    int ops_done = 0;
    double t0 = now_sec();

    for (int i = 0; i < g_ops; i++) {
        double s = now_sec();
        MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                     ts_oracle_next(&db->mvcc_oracle),
                                     NULL, 0);
        mvcc_registry_add(&db->mvcc_registry, tx);
        Document *d = make_bench_doc(1, i);
        int rc = mvcc_update_doc(db, tx, BENCH_COLL, 1, d);
        doc_free(d);
        if (rc == MVCC_OK) {
            mvcc_commit_tx(db, tx);
            ops_done++;
        } else {
            mvcc_abort_tx(db, tx);
            aborts++;
        }
        mvcc_tx_free(tx);
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "MVCC", "W2_hotwrite", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), aborts };
    lat_free(&lat);
    return r;
}

/* ===================================================================
 * Workload W3: 100% writes, many keys
 * =================================================================== */

static BenchResult bench_w3_2pl(DiskDB *db) {
    DiskColl *c = ddb_get_coll(db, BENCH_COLL);
    LatTracker lat; lat_init(&lat);
    double t0 = now_sec();
    int ops_done = 0;

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();
        Document *d = make_bench_doc((int)doc_id, i);
        uint64_t tx = wal_new_tx_id(&db->wal);
        wal_log_begin(&db->wal, tx);
        int rc = ddb_update_doc(db, c, doc_id, d);
        doc_free(d);
        if (rc == 0) { wal_log_commit(&db->wal, tx); ops_done++; }
        else            wal_log_abort(&db->wal, tx);
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "2PL", "W3_multiwrite", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), 0 };
    lat_free(&lat);
    return r;
}

static BenchResult bench_w3_mvcc(DiskDB *db) {
    LatTracker lat; lat_init(&lat);
    int aborts = 0, ops_done = 0;
    double t0 = now_sec();

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();
        MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                     ts_oracle_next(&db->mvcc_oracle),
                                     NULL, 0);
        mvcc_registry_add(&db->mvcc_registry, tx);
        Document *d = make_bench_doc((int)doc_id, i);
        int rc = mvcc_update_doc(db, tx, BENCH_COLL, doc_id, d);
        doc_free(d);
        if (rc == MVCC_OK) { mvcc_commit_tx(db, tx); ops_done++; }
        else                { mvcc_abort_tx(db, tx);  aborts++;   }
        mvcc_tx_free(tx);
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "MVCC", "W3_multiwrite", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), aborts };
    lat_free(&lat);
    return r;
}

/* ===================================================================
 * Workload W4: Mixed 50% reads + 50% writes
 * =================================================================== */

static BenchResult bench_w4_2pl(DiskDB *db) {
    DiskColl *c = ddb_get_coll(db, BENCH_COLL);
    LatTracker lat; lat_init(&lat);
    double t0 = now_sec();
    int ops_done = 0;

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();
        if (rand() % 2 == 0) {
            Document *d = ddb_read_doc(db, c, doc_id);
            if (d) doc_free(d);
        } else {
            Document *d = make_bench_doc((int)doc_id, i);
            ddb_update_doc(db, c, doc_id, d);
            doc_free(d);
        }
        ops_done++;
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "2PL", "W4_mixed", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), 0 };
    lat_free(&lat);
    return r;
}

static BenchResult bench_w4_mvcc(DiskDB *db) {
    LatTracker lat; lat_init(&lat);
    int aborts = 0, ops_done = 0;
    double t0 = now_sec();

    for (int i = 0; i < g_ops; i++) {
        uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
        double s = now_sec();
        MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                     ts_oracle_next(&db->mvcc_oracle),
                                     NULL, 0);
        mvcc_registry_add(&db->mvcc_registry, tx);
        if (rand() % 2 == 0) {
            int err;
            Document *d = mvcc_find_doc(db, tx, BENCH_COLL, doc_id, &err);
            if (d) doc_free(d);
            mvcc_commit_tx(db, tx);
        } else {
            Document *d = make_bench_doc((int)doc_id, i);
            int rc = mvcc_update_doc(db, tx, BENCH_COLL, doc_id, d);
            doc_free(d);
            if (rc == MVCC_OK) { mvcc_commit_tx(db, tx); }
            else                { mvcc_abort_tx(db, tx); aborts++; }
        }
        ops_done++;
        mvcc_tx_free(tx);
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "MVCC", "W4_mixed", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), aborts };
    lat_free(&lat);
    return r;
}

/* ===================================================================
 * Workload W5: Long-running reader + many writes
 * Measures: reader không bị block bởi writers (MVCC advantage)
 * =================================================================== */

static BenchResult bench_w5_2pl(DiskDB *db) {
    /* 2PL single-thread: reader và writer sequential → reader luôn "block" */
    DiskColl *c = ddb_get_coll(db, BENCH_COLL);
    LatTracker lat; lat_init(&lat);
    double t0 = now_sec();
    int ops_done = 0;

    /* Simulate: 1 read per N writes (reader "waiting") */
    int WRITE_BATCH = 10;
    for (int i = 0; i < g_ops; i++) {
        double s = now_sec();
        if (i % (WRITE_BATCH + 1) == 0) {
            /* Reader phải đợi toàn bộ writes xong (sequential) */
            Document *d = ddb_read_doc(db, c, 1);
            if (d) doc_free(d);
        } else {
            uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
            Document *d = make_bench_doc((int)doc_id, i);
            ddb_update_doc(db, c, doc_id, d);
            doc_free(d);
        }
        ops_done++;
        lat_record(&lat, now_sec() - s);
    }
    double elapsed = now_sec() - t0;
    BenchResult r = { "2PL", "W5_longreader", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), 0 };
    lat_free(&lat);
    return r;
}

static BenchResult bench_w5_mvcc(DiskDB *db) {
    /* MVCC: reader dùng snapshot cũ, không bị block bởi writers */
    LatTracker lat; lat_init(&lat);
    int aborts = 0, ops_done = 0;
    double t0 = now_sec();

    /* Long-running reader tx — bắt đầu từ đầu, giữ snapshot */
    MvccTx *reader = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                     ts_oracle_next(&db->mvcc_oracle),
                                     NULL, 0);
    mvcc_registry_add(&db->mvcc_registry, reader);

    int WRITE_BATCH = 10;
    for (int i = 0; i < g_ops; i++) {
        double s = now_sec();
        if (i % (WRITE_BATCH + 1) == 0) {
            /* Reader đọc snapshot của nó — không bị block */
            int err;
            Document *d = mvcc_find_doc(db, reader, BENCH_COLL, 1, &err);
            if (d) doc_free(d);
        } else {
            uint64_t doc_id = (uint64_t)(rand() % g_docs) + 1;
            MvccTx *wtx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                          ts_oracle_next(&db->mvcc_oracle),
                                          NULL, 0);
            mvcc_registry_add(&db->mvcc_registry, wtx);
            Document *d = make_bench_doc((int)doc_id, i);
            int rc = mvcc_update_doc(db, wtx, BENCH_COLL, doc_id, d);
            doc_free(d);
            if (rc == MVCC_OK) { mvcc_commit_tx(db, wtx); }
            else                { mvcc_abort_tx(db, wtx); aborts++; }
            mvcc_tx_free(wtx);
        }
        ops_done++;
        lat_record(&lat, now_sec() - s);
    }

    /* Commit long reader */
    mvcc_commit_tx(db, reader);
    mvcc_tx_free(reader);

    double elapsed = now_sec() - t0;
    BenchResult r = { "MVCC", "W5_longreader", ops_done, elapsed,
                      ops_done / elapsed,
                      lat_percentile(&lat, 50), lat_percentile(&lat, 99), aborts };
    lat_free(&lat);
    return r;
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(int argc, char **argv) {
    if (argc >= 2) g_ops  = atoi(argv[1]);
    if (argc >= 3) g_docs = atoi(argv[2]);
    if (g_ops  <= 0) g_ops  = 5000;
    if (g_docs <= 0) g_docs = 1000;

    srand(42);  /* reproducible */

    fprintf(stderr, "=== Hugo DB Benchmark: 2PL vs MVCC ===\n");
    fprintf(stderr, "ops=%d  seed_docs=%d\n\n", g_ops, g_docs);

    /* CSV header */
    printf("mode,workload,ops,elapsed_sec,throughput_ops_per_sec,p50_us,p99_us,aborts\n");

    /* ===== Setup 2PL DB ===== */
    remove(BENCH_DB_2PL);
    remove(BENCH_DB_2PL ".hugolog");
    DiskDB db2pl;
    ddb_create(&db2pl, "bench_2pl", BENCH_DB_2PL);
    seed_db_2pl(&db2pl);

    /* ===== Setup MVCC DB ===== */
    remove(BENCH_DB_MVCC);
    remove(BENCH_DB_MVCC ".hugolog");
    DiskDB dbmvcc;
    ddb_create(&dbmvcc, "bench_mvcc", BENCH_DB_MVCC);
    dbmvcc.mode = HUGO_MODE_MVCC;
    seed_db_mvcc(&dbmvcc);

    /* ===== Run workloads ===== */
    BenchResult results[10];
    int nr = 0;

    fprintf(stderr, "W1: 100%% reads...\n");
    results[nr++] = bench_w1_2pl(&db2pl);
    results[nr++] = bench_w1_mvcc(&dbmvcc);

    fprintf(stderr, "W2: hot key writes...\n");
    results[nr++] = bench_w2_2pl(&db2pl);
    results[nr++] = bench_w2_mvcc(&dbmvcc);

    fprintf(stderr, "W3: multi-key writes...\n");
    results[nr++] = bench_w3_2pl(&db2pl);
    results[nr++] = bench_w3_mvcc(&dbmvcc);

    fprintf(stderr, "W4: mixed 50/50...\n");
    results[nr++] = bench_w4_2pl(&db2pl);
    results[nr++] = bench_w4_mvcc(&dbmvcc);

    fprintf(stderr, "W5: long reader + writers...\n");
    results[nr++] = bench_w5_2pl(&db2pl);
    results[nr++] = bench_w5_mvcc(&dbmvcc);

    /* Print CSV */
    for (int i = 0; i < nr; i++) {
        print_result(stdout, &results[i]);
    }

    /* Summary table to stderr */
    fprintf(stderr, "\n%-6s %-15s %8s %10s %8s %8s %6s\n",
            "Mode", "Workload", "ops/sec", "elapsed", "p50µs", "p99µs", "aborts");
    fprintf(stderr, "%-6s %-15s %8s %10s %8s %8s %6s\n",
            "------", "---------------", "--------", "----------",
            "--------", "--------", "------");
    for (int i = 0; i < nr; i++) {
        BenchResult *r = &results[i];
        fprintf(stderr, "%-6s %-15s %8.0f %10.3fs %8.1f %8.1f %6d\n",
                r->mode, r->workload, r->throughput,
                r->elapsed_sec, r->p50_us, r->p99_us, r->aborts);
        if (i % 2 == 1) fprintf(stderr, "\n");
    }

    ddb_close(&db2pl);
    ddb_close(&dbmvcc);
    remove(BENCH_DB_2PL);
    remove(BENCH_DB_2PL ".hugolog");
    remove(BENCH_DB_MVCC);
    remove(BENCH_DB_MVCC ".hugolog");

    return 0;
}
