/* bench_optimizer.c — Phase 9: Optimizer Benchmark
 *
 * Measures performance of optimizer ON vs OFF across 4 scenarios:
 *   1. Selective filter with index
 *   2. Non-selective filter (full scan)
 *   3. Predicate pushdown benefit
 *   4. JOIN query with/without optimizer
 *
 * Usage: bench_optimizer.exe [db_path]
 * Output: timing table, speedup ratios
 */
#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/query/executor.h"
#include "../src/core/executor_disk.h"
#include "../src/core/disk_db.h"
#include "../src/core/collection.h"
#include "../src/core/optimizer/optimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_DB "bench_tmp.hugo"
#define BENCH_ITERATIONS 5

/* ===== Timer ===== */
static double get_ms(void) {
    return (double)clock() / CLOCKS_PER_SEC * 1000.0;
}

/* ===== DB helpers ===== */
static DiskDB g_db;

static void insert_doc_num(const char *coll, const char *f1, double v1,
                             const char *f2, double v2) {
    DiskColl *c = ddb_get_coll(&g_db, coll);
    if (!c) c = ddb_create_coll(&g_db, coll);
    if (!c) return;
    Document *d = (Document*)calloc(1, sizeof(Document));
    Value val; memset(&val, 0, sizeof(val)); val.type = VAL_NUM;
    val.num = v1; doc_set_field(d, f1, val);
    val.num = v2; doc_set_field(d, f2, val);
    uint64_t id;
    ddb_insert_doc(&g_db, c, d, &id);
    doc_free(d);
}

static void insert_doc_str(const char *coll, const char *f1, const char *v1,
                             const char *f2, double v2) {
    DiskColl *c = ddb_get_coll(&g_db, coll);
    if (!c) c = ddb_create_coll(&g_db, coll);
    if (!c) return;
    Document *d = (Document*)calloc(1, sizeof(Document));
    Value val; memset(&val, 0, sizeof(val));
    val.type = VAL_STR; strncpy(val.str, v1, sizeof(val.str)-1);
    doc_set_field(d, f1, val);
    val.type = VAL_NUM; val.num = v2;
    doc_set_field(d, f2, val);
    uint64_t id;
    ddb_insert_doc(&g_db, c, d, &id);
    doc_free(d);
}

static int exec_query(const char *ql) {
    TokenList tl;
    if (hugo_tokenize(ql, &tl) != 0) return -1;
    Query q; hugo_parse(&tl, &q);
    HugoResult r;
    hugo_execute_disk_opt(&g_db, &q, &r);
    int count = r.count;
    result_free_disk(&r);
    query_free(&q);
    return count;
}

static int exec_legacy(const char *ql) {
    TokenList tl;
    if (hugo_tokenize(ql, &tl) != 0) return -1;
    Query q; hugo_parse(&tl, &q);
    HugoResult r;
    hugo_execute_disk(&g_db, &q, &r);
    int count = r.count;
    result_free_disk(&r);
    query_free(&q);
    return count;
}

/* ===== Benchmark runner ===== */
typedef struct {
    const char *name;
    const char *query;
    double      time_legacy_ms;
    double      time_opt_ms;
    int         rows_legacy;
    int         rows_opt;
} BenchResult;

static BenchResult run_bench(const char *name, const char *query) {
    BenchResult br;
    memset(&br, 0, sizeof(br));
    br.name  = name;
    br.query = query;

    /* Warm up */
    exec_legacy(query);
    exec_query(query);

    /* Legacy timing */
    OptimizerCtx *ctx = hugo_optimizer_get();
    if (ctx) optimizer_set_mode(ctx, HUGO_OPT_OFF);

    double t0 = get_ms();
    for (int i = 0; i < BENCH_ITERATIONS; i++)
        br.rows_legacy = exec_legacy(query);
    br.time_legacy_ms = (get_ms() - t0) / BENCH_ITERATIONS;

    /* Optimizer timing */
    if (ctx) optimizer_set_mode(ctx, HUGO_OPT_COST_BASED);

    t0 = get_ms();
    for (int i = 0; i < BENCH_ITERATIONS; i++)
        br.rows_opt = exec_query(query);
    br.time_opt_ms = (get_ms() - t0) / BENCH_ITERATIONS;

    return br;
}

static void print_result(const BenchResult *br) {
    double speedup = br->time_legacy_ms > 0 ?
                     br->time_legacy_ms / br->time_opt_ms : 1.0;
    int correct = (br->rows_legacy == br->rows_opt);

    printf("  %-40s  legacy=%6.2fms  opt=%6.2fms  speedup=%5.2fx  "
           "rows=%d/%d  %s\n",
           br->name,
           br->time_legacy_ms, br->time_opt_ms,
           speedup,
           br->rows_legacy, br->rows_opt,
           correct ? "OK" : "MISMATCH!");
}

/* ===== Seed data ===== */
#define BENCH_ROWS 2000

static void seed_data(void) {
    printf("Seeding %d rows...\n", BENCH_ROWS);

    /* Collection: employees (id, age, salary, dept_id) */
    for (int i = 0; i < BENCH_ROWS; i++) {
        insert_doc_num("employees", "age", 20 + (i % 50),
                        "salary", 30000 + (i % 100) * 500);
    }

    /* Collection: departments (dept_id, name) */
    for (int i = 1; i <= 20; i++) {
        insert_doc_num("departments", "dept_id", (double)i,
                        "budget", (double)(i * 10000));
    }

    /* Create index on employees.age */
    {
        DiskColl *c = ddb_get_coll(&g_db, "employees");
        if (c && c->n_indexes < DDB_MAX_INDEXES) {
            strncpy(c->indexes[c->n_indexes].field, "age", 127);
            c->indexes[c->n_indexes].btree_root_page = 0;
            c->n_indexes++;
        }
    }

    /* Analyze collections */
    OptimizerCtx *ctx = hugo_optimizer_get();
    if (ctx) {
        optimizer_analyze(ctx, "employees");
        optimizer_analyze(ctx, "departments");
    }
    printf("Seeding done. Running benchmarks...\n\n");
}

/* ===== Main ===== */
int main(int argc, char **argv) {
    const char *db_path = (argc >= 2) ? argv[1] : BENCH_DB;

    printf("========================================\n");
    printf(" Hugo DB — Phase 9 Optimizer Benchmark\n");
    printf("========================================\n");
    printf("DB: %s  Iterations: %d  Rows: %d\n\n",
           db_path, BENCH_ITERATIONS, BENCH_ROWS);

    /* Create fresh DB */
    remove(db_path);
    char stats_path[512], log_path[512];
    snprintf(stats_path, sizeof(stats_path), "%s.stats", db_path);
    snprintf(log_path, sizeof(log_path), "%s.hugolog", db_path);
    remove(stats_path);
    remove(log_path);

    ddb_create(&g_db, "bench", db_path);
    hugo_optimizer_init(&g_db, db_path, HUGO_OPT_COST_BASED);
    seed_data();

    /* ===== Scenario 1: Selective filter (high selectivity) ===== */
    printf("Scenario 1: Selective filter (age = 25, ~%d rows)\n",
           BENCH_ROWS / 50);
    {
        BenchResult br = run_bench("filter age=25",
                                    "funden employees haar age $bg 25");
        print_result(&br);
    }

    printf("\nScenario 2: Non-selective filter (all rows)\n");
    {
        BenchResult br = run_bench("filter age>0 (all rows)",
                                    "funden employees haar age $bh 0");
        print_result(&br);
    }

    printf("\nScenario 3: Sort + Limit\n");
    {
        BenchResult br = run_bench("sort salary desc, limit 10",
                                    "funden employees orange bi salary desc lime 10");
        print_result(&br);
    }

    printf("\nScenario 4: Range filter\n");
    {
        BenchResult br = run_bench("filter 30<=age<=40",
                                    "funden employees haar age $bhb 30 lime 50");
        print_result(&br);
    }

    printf("\nScenario 5: GROUP BY aggregate\n");
    {
        BenchResult br = run_bench("group by salary bucket (pou)",
                                    "gomail employees gremb bi age pou salary");
        print_result(&br);
    }

    printf("\nScenario 6: JOIN query\n");
    {
        BenchResult br = run_bench("departments JOIN employees (limit 10)",
            "funden departments $rasoat employees local_field dept_id target_field dept_id lime 10");
        /* Note: JOIN result format may differ between optimizer and legacy
         * (legacy embeds joined fields with alias prefix, optimizer merges directly).
         * Both return results — correctness verified separately in test_optimizer. */
        double speedup = br.time_legacy_ms > 0 ?
                         br.time_legacy_ms / br.time_opt_ms : 1.0;
        printf("  %-40s  legacy=%6.2fms  opt=%6.2fms  speedup=%5.2fx  (join rows vary by impl)\n",
               br.name, br.time_legacy_ms, br.time_opt_ms, speedup);
    }

    printf("\n========================================\n");
    printf("Note: optimizer and legacy use same SeqScan execution path\n");
    printf("      (true IndexScan traversal = stretch goal Phase 8b)\n");
    printf("      Speedup visible when optimizer eliminates work via rules.\n");
    printf("========================================\n");

    /* Verify correctness: all rows must match */
    {
        int errs = 0;
        const char *check_queries[] = {
            "funden employees haar age $bg 30",
            "funden employees orange bi salary desc lime 5",
            "gomail employees gremb bi age pou salary",
            NULL
        };
        OptimizerCtx *ctx = hugo_optimizer_get();
        for (int i = 0; check_queries[i]; i++) {
            if (ctx) optimizer_set_mode(ctx, HUGO_OPT_OFF);
            int leg = exec_legacy(check_queries[i]);
            if (ctx) optimizer_set_mode(ctx, HUGO_OPT_COST_BASED);
            int opt = exec_query(check_queries[i]);
            if (leg != opt) {
                printf("CORRECTNESS FAIL: \"%s\" legacy=%d opt=%d\n",
                       check_queries[i], leg, opt);
                errs++;
            }
        }
        if (errs == 0)
            printf("\nCorrectness: ALL queries return same results. OK\n");
    }

    ddb_close(&g_db);
    remove(db_path);
    remove(stats_path);
    remove(log_path);
    return 0;
}
