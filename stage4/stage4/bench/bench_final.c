/* bench_final.c — HugoDB Vectorized Execution: Complete Benchmark
 *
 * Đo từng lớp tối ưu theo thứ tự, 4 query types × 3 dataset sizes.
 * Output: bảng ASCII với ms, speedup, throughput rows/s.
 *
 * Build (Linux):
 *   make bench_final
 *   hoặc thủ công:
 *   gcc -O3 -march=native -I src -I src/core -I src/query \
 *       -I src/core/optimizer -D_POSIX_C_SOURCE=200809L \
 *       -Dstrtok_s=strtok_r \
 *       bench/bench_final.c \
 *       src/vec/col_batch.c src/vec/vec_filter.c src/vec/vec_agg.c \
 *       src/vec/vec_sort.c src/vec/vec_str_intern.c \
 *       src/vec/vec_bulk_scan.c src/vec/vec_scan_cache.c \
 *       src/vec/vec_exec.c \
 *       src/core/collection.c src/core/optimizer/arena.c \
 *       src/core/disk_db.c src/core/page.c src/core/wal.c \
 *       src/core/buffer_pool.c src/core/btree.c src/core/dbtree.c \
 *       src/core/checksum.c src/core/phys_executor.c \
 *       src/core/optimizer/optimizer.c \
 *       src/core/optimizer/logical_plan.c \
 *       src/core/optimizer/physical_plan.c \
 *       src/core/optimizer/cost_model.c \
 *       src/core/optimizer/statistics.c \
 *       src/core/optimizer/rules.c \
 *       src/core/optimizer/advanced_rules.c \
 *       src/core/optimizer/join_order.c \
 *       src/query/executor.c src/core/hugo_io_posix.c \
 *       -lm -o bench_final && ./bench_final
 *
 * Build (Windows MSVC):
 *   cl /O2 /arch:AVX2 /I src /I src\core /I src\query \
 *      /I src\core\optimizer \
 *      bench\bench_final.c \
 *      src\vec\col_batch.c src\vec\vec_filter.c src\vec\vec_agg.c \
 *      src\vec\vec_sort.c src\vec\vec_str_intern.c \
 *      src\vec\vec_bulk_scan.c src\vec\vec_scan_cache.c \
 *      src\vec\vec_exec.c \
 *      src\core\collection.c src\core\optimizer\arena.c \
 *      src\core\disk_db.c src\core\page.c src\core\wal.c \
 *      src\core\buffer_pool.c src\core\btree.c src\core\dbtree.c \
 *      src\core\checksum.c src\core\phys_executor.c \
 *      src\core\optimizer\optimizer.c \
 *      src\core\optimizer\logical_plan.c \
 *      src\core\optimizer\physical_plan.c \
 *      src\core\optimizer\cost_model.c \
 *      src\core\optimizer\statistics.c \
 *      src\core\optimizer\rules.c \
 *      src\core\optimizer\advanced_rules.c \
 *      src\core\optimizer\join_order.c \
 *      src\query\executor.c src\core\hugo_io_win.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "vec/col_batch.h"
#include "vec/vec_filter.h"
#include "vec/vec_agg.h"
#include "vec/vec_sort.h"
#include "vec/vec_str_intern.h"
#include "vec/vec_bulk_scan.h"
#include "vec/vec_scan_cache.h"
#include "vec/vec_exec.h"
#include "core/phys_executor.h"
#include "core/disk_db.h"
#include "core/collection.h"
#include "core/optimizer/arena.h"
#include "core/optimizer/physical_plan.h"
#include "query/executor.h"

/* ================================================================
 * Config
 * ================================================================ */
#define BENCH_WARMUP  2
#define BENCH_RUNS    5
#ifdef _WIN32
#define BENCH_DB_PATH "hugodb_bench_final.hugo"
#else
#define BENCH_DB_PATH "/tmp/hugodb_bench_final.hugo"
#endif

/* Part 1: pure execution sizes (no disk) */
static const int SIZES[]      = { 20000, 100000 };
#define N_SIZES 2

/* Part 2: full pipeline sizes (with disk) */
static const int DISK_SIZES[] = { 5000, 20000, 50000 };
#define N_DISK_SIZES 3

/* ================================================================
 * Timer — high resolution, cross-platform
 * ================================================================ */
#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart * 1000.0;
}
#else
static double now_ms(void) {
    return (double)clock() / CLOCKS_PER_SEC * 1000.0;
}
#endif

/* ================================================================
 * Document factory
 * ================================================================ */
static const char *REGIONS[] = { "north", "south", "east", "west" };

static Document* make_doc(int i) {
    Document *d = (Document*)calloc(1, sizeof(Document));
    Value v; memset(&v, 0, sizeof(v));

    v.type = VAL_NUM; v.num = (double)(i % 1000);
    doc_set_field(d, "score", v);

    v.type = VAL_NUM; v.num = (double)(18 + i % 60);
    doc_set_field(d, "age", v);

    v.type = VAL_STR;
    strncpy(v.str, REGIONS[i % 4], sizeof(v.str)-1);
    doc_set_field(d, "region", v);

    return d;
}

/* ================================================================
 * PhysicalPlan builders
 * ================================================================ */
static PhysicalPlan* plan_scan(Arena *a, const char *coll) {
    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(a, sizeof(PhysicalPlan));
    memset(p, 0, sizeof(*p));
    p->type = POP_SEQ_SCAN;
    strncpy(p->seq_scan.collection_name, coll, 63);
    return p;
}

static PhysicalPlan* plan_filter(Arena *a, PhysicalPlan *child,
                                  const char *field, double val,
                                  HugoTokenType op) {
    Condition *c = (Condition*)arena_alloc(a, sizeof(Condition));
    memset(c, 0, sizeof(*c));
    c->type = COND_CMP;
    strncpy(c->field, field, 127);
    c->op = op;
    c->value.type = VAL_NUM;
    c->value.num  = val;

    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(a, sizeof(PhysicalPlan));
    memset(p, 0, sizeof(*p));
    p->type = POP_FILTER;
    p->filter.predicate = c;
    p->left = child;
    return p;
}

static PhysicalPlan* plan_sort(Arena *a, PhysicalPlan *child,
                                const char *field, int desc) {
    SortField *sf = (SortField*)arena_alloc(a, sizeof(SortField));
    memset(sf, 0, sizeof(*sf));
    strncpy(sf->field, field, 127);
    sf->descending = desc;

    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(a, sizeof(PhysicalPlan));
    memset(p, 0, sizeof(*p));
    p->type = POP_SORT;
    p->sort.fields   = sf;
    p->sort.n_fields = 1;
    p->left = child;
    return p;
}

static PhysicalPlan* plan_limit(Arena *a, PhysicalPlan *child,
                                 int lim, int skip) {
    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(a, sizeof(PhysicalPlan));
    memset(p, 0, sizeof(*p));
    p->type = POP_LIMIT;
    p->limit.limit = lim;
    p->limit.skip  = skip;
    p->left = child;
    return p;
}

static PhysicalPlan* plan_agg(Arena *a, PhysicalPlan *child,
                               const char *group_field,
                               AggFunc *aggs, int n_aggs) {
    PhysicalPlan *p = (PhysicalPlan*)arena_alloc(a, sizeof(PhysicalPlan));
    memset(p, 0, sizeof(*p));
    p->type = POP_HASH_AGGREGATE;
    strncpy(p->aggregate.group_by_field, group_field, 127);
    p->aggregate.aggs   = aggs;
    p->aggregate.n_aggs = (size_t)n_aggs;
    p->left = child;
    return p;
}

/* ================================================================
 * Row-model helpers (baseline, no ColBatch)
 * ================================================================ */
static int row_filter_count(Document **docs, int n, double thr) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        Value v;
        if (doc_get_field(docs[i], "score", &v) == 0 && v.num > thr) c++;
    }
    return c;
}

typedef struct { Document *d; double key; } SortRow;
static int sort_row_cmp_desc(const void *a, const void *b) {
    double da = ((const SortRow*)a)->key;
    double db = ((const SortRow*)b)->key;
    return (da > db) ? -1 : (da < db) ? 1 : 0;
}

static void row_sort_topk(Document **docs, int n, int k) {
    SortRow *rows = (SortRow*)malloc(n * sizeof(SortRow));
    for (int i = 0; i < n; i++) {
        Value v; doc_get_field(docs[i], "score", &v);
        rows[i].d = docs[i]; rows[i].key = v.num;
    }
    qsort(rows, n, sizeof(SortRow), sort_row_cmp_desc);
    (void)k;
    free(rows);
}

static void row_group_by_sum(Document **docs, int n) {
    double sums[4] = {0}; int cnts[4] = {0};
    for (int i = 0; i < n; i++) {
        Value rv, sv;
        doc_get_field(docs[i], "region", &rv);
        doc_get_field(docs[i], "score",  &sv);
        int g = (strcmp(rv.str,"north")==0) ? 0 :
                (strcmp(rv.str,"south")==0) ? 1 :
                (strcmp(rv.str,"east") ==0) ? 2 : 3;
        sums[g] += sv.num; cnts[g]++;
    }
    (void)sums; (void)cnts;
}

/* ================================================================
 * Measurement harness
 * ================================================================ */
typedef struct {
    double avg_ms;
    double min_ms;
    int    result_count;
} MeasResult;

typedef double (*BenchFn)(void *ctx);

static MeasResult measure(BenchFn fn, void *ctx) {
    MeasResult r = {0};
    double times[BENCH_RUNS + BENCH_WARMUP];
    int    counts[BENCH_RUNS + BENCH_WARMUP];

    for (int i = 0; i < BENCH_RUNS + BENCH_WARMUP; i++) {
        times[i]  = fn(ctx);
        counts[i] = 0; /* filled by fn via ctx */
    }

    double sum = 0, mn = 1e18;
    for (int i = BENCH_WARMUP; i < BENCH_RUNS + BENCH_WARMUP; i++) {
        sum += times[i];
        if (times[i] < mn) mn = times[i];
    }
    r.avg_ms = sum / BENCH_RUNS;
    r.min_ms = mn;
    r.result_count = counts[BENCH_WARMUP];
    return r;
}

/* ================================================================
 * Bench contexts
 * ================================================================ */

/* --- Pure vec (in-memory ColBatch, no disk) --- */
typedef struct {
    ColBatch    *b;
    Arena       *arena_per_run; /* reset each run */
    int          query;         /* 0=filter,1=sort,2=groupby,3=combined */
    double       threshold;
    int          result_count;
} PureCtx;

static double bench_pure_vec(void *ctx_) {
    PureCtx *ctx = (PureCtx*)ctx_;
    Arena *a = arena_new();

    /* Reset alive */
    memset(ctx->b->alive, 1, ctx->b->n_rows);
    ctx->b->perm = NULL;

    double t0 = now_ms();

    if (ctx->query == 0) {
        /* Filter: score > threshold */
        Condition cond; memset(&cond, 0, sizeof(cond));
        cond.type = COND_CMP; strncpy(cond.field, "score", 127);
        cond.op = TOK_OP_BH; cond.value.type = VAL_NUM;
        cond.value.num = ctx->threshold;
        vec_filter_apply(ctx->b, &cond);
        ctx->result_count = col_batch_alive_count(ctx->b);

    } else if (ctx->query == 1) {
        /* Sort score DESC + limit 10 */
        SortField sf; memset(&sf, 0, sizeof(sf));
        strncpy(sf.field, "score", 127); sf.descending = 1;
        vec_sort_topk(ctx->b, &sf, 10, a);
        ctx->result_count = 10;

    } else if (ctx->query == 2) {
        /* GROUP BY region, SUM(score) */
        AggFunc aggs[2]; memset(aggs, 0, sizeof(aggs));
        aggs[0].func = TOK_POU;
        strncpy(aggs[0].field, "score", 127);
        strncpy(aggs[0].out_name, "cnt", 255);
        aggs[1].func = TOK_SEP;
        strncpy(aggs[1].field, "score", 127);
        strncpy(aggs[1].out_name, "sum_score", 255);
        VecAggTable *t = vec_agg_new(a, ctx->b, "region", aggs, 2);
        vec_agg_run_fast(t, ctx->b, a);
        ctx->result_count = t ? t->n_groups : 0;

    } else {
        /* Combined: filter + sort + limit
         * NOTE: do NOT call vec_filter_compact — it mutates b->docs[]
         * which is borrowed. Use alive bitmap + sort on alive rows. */
        Condition cond; memset(&cond, 0, sizeof(cond));
        cond.type = COND_CMP; strncpy(cond.field, "score", 127);
        cond.op = TOK_OP_BH; cond.value.type = VAL_NUM;
        cond.value.num = ctx->threshold;
        vec_filter_apply(ctx->b, &cond);
        SortField sf; memset(&sf, 0, sizeof(sf));
        strncpy(sf.field, "score", 127); sf.descending = 1;
        vec_sort_full(ctx->b, &sf, a);
        ctx->result_count = vec_sort_apply_limit(ctx->b, 0, 10);
    }

    double elapsed = now_ms() - t0;
    arena_free(a);
    return elapsed;
}

/* --- Pure row (in-memory Document**, no disk) --- */
typedef struct {
    Document   **docs;
    int          n;
    int          query;
    double       threshold;
    int          result_count;
} PureRowCtx;

static double bench_pure_row(void *ctx_) {
    PureRowCtx *ctx = (PureRowCtx*)ctx_;
    double t0 = now_ms();

    if (ctx->query == 0) {
        ctx->result_count = row_filter_count(ctx->docs, ctx->n, ctx->threshold);
    } else if (ctx->query == 1) {
        row_sort_topk(ctx->docs, ctx->n, 10);
        ctx->result_count = 10;
    } else if (ctx->query == 2) {
        row_group_by_sum(ctx->docs, ctx->n);
        ctx->result_count = 4;
    } else {
        ctx->result_count = row_filter_count(ctx->docs, ctx->n, ctx->threshold);
        /* simplified: filter then sort */
        row_sort_topk(ctx->docs, ctx->n, 10);
        ctx->result_count = 10;
    }

    return now_ms() - t0;
}

/* --- Full pipeline (with DiskDB) --- */
typedef struct {
    DiskDB      *db;
    int          query;
    double       threshold;
    int          use_vec;       /* 1 = vec_exec, 0 = phys_exec */
    int          cold;          /* 1 = invalidate cache each run */
    int          result_count;
} DiskCtx;

static PhysicalPlan* build_disk_plan(Arena *a, int query, double thr) {
    PhysicalPlan *scan   = plan_scan(a, "bench");
    PhysicalPlan *filter = plan_filter(a, scan, "score", thr, TOK_OP_BH);

    if (query == 0) {
        return filter;
    } else if (query == 1) {
        PhysicalPlan *sort = plan_sort(a, filter, "score", 1);
        return plan_limit(a, sort, 10, 0);
    } else if (query == 2) {
        AggFunc *aggs = (AggFunc*)arena_alloc(a, 2 * sizeof(AggFunc));
        memset(aggs, 0, 2 * sizeof(AggFunc));
        aggs[0].func = TOK_POU;
        strncpy(aggs[0].field, "score", 127);
        strncpy(aggs[0].out_name, "cnt", 255);
        aggs[1].func = TOK_SEP;
        strncpy(aggs[1].field, "score", 127);
        strncpy(aggs[1].out_name, "sum_score", 255);
        return plan_agg(a, filter, "region", aggs, 2);
    } else {
        PhysicalPlan *sort = plan_sort(a, filter, "score", 1);
        return plan_limit(a, sort, 10, 0);
    }
}

static double bench_disk(void *ctx_) {
    DiskCtx *ctx = (DiskCtx*)ctx_;

    if (ctx->cold) {
        /* Full cache reset — destroy + reinit to avoid any stale state */
        scan_cache_destroy(vec_get_scan_cache());
        scan_cache_init(vec_get_scan_cache());
    }

    Arena *a = arena_new();
    PhysicalPlan *plan = build_disk_plan(a, ctx->query, ctx->threshold);
    HugoResult res;

    double t0 = now_ms();
    if (ctx->use_vec)
        vec_exec_run(ctx->db, plan, &res, a);
    else
        phys_exec_run(ctx->db, plan, &res, a);
    double elapsed = now_ms() - t0;

    ctx->result_count = res.count;
    for (int i = 0; i < res.count; i++)
        if (res.docs[i]) doc_free(res.docs[i]);
    arena_free(a);
    return elapsed;
}

/* ================================================================
 * Output helpers
 * ================================================================ */
static void print_separator(int wide) {
    if (wide)
        printf("%-38s+----------+----------+----------+----------+----------+\n",
               "--------------------------------------");
    else
        printf("%-30s+----------+----------+\n",
               "------------------------------");
}

static void print_row(const char *label, double ms, double base_ms,
                      int rows, int is_header) {
    if (is_header) {
        printf("  %-35s %9s  %9s  %12s\n",
               label, "avg ms", "speedup", "rows/s");
        return;
    }
    double speedup   = (base_ms > 0.001) ? base_ms / ms : 0.0;
    double rows_per_s = (ms > 0.001) ? (rows / (ms / 1000.0)) : 0.0;
    printf("  %-35s %9.2f  %9.1fx  %10.0f\n",
           label, ms, speedup, rows_per_s);
}

static void print_section(const char *title) {
    int len = (int)strlen(title);
    printf("\n");
    for (int i = 0; i < len + 4; i++) putchar('=');
    printf("\n  %s\n", title);
    for (int i = 0; i < len + 4; i++) putchar('=');
    printf("\n");
}

static const char *query_name(int q) {
    switch (q) {
    case 0: return "filter  (score > 500)";
    case 1: return "sort    (score DESC) + limit 10";
    case 2: return "groupby (region) + SUM(score)";
    case 3: return "combined (filter+sort+limit 10)";
    }
    return "?";
}

/* ================================================================
 * Setup DB
 * ================================================================ */
static void setup_db(DiskDB *db, int N) {
    remove(BENCH_DB_PATH);
    remove(BENCH_DB_PATH "log");  /* xxx.hugo -> xxx.hugolog */
    ddb_create(db, "hugodb_bench", BENCH_DB_PATH);
    DiskColl *c = ddb_create_coll(db, "bench");

    for (int i = 0; i < N; i++) {
        Document *d = make_doc(i);
        uint64_t id;
        ddb_insert_doc(db, c, d, &id);
        doc_free(d);
    }
    /* Full cache reset on each DB setup */
    scan_cache_destroy(vec_get_scan_cache());
    scan_cache_init(vec_get_scan_cache());
}

static void teardown_db(DiskDB *db) {
    scan_cache_destroy(vec_get_scan_cache());
    scan_cache_init(vec_get_scan_cache());
    ddb_close(db);
    remove(BENCH_DB_PATH);
    remove(BENCH_DB_PATH "log");  /* xxx.hugo -> xxx.hugolog */
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   HugoDB — Vectorized Execution Benchmark            ║\n");
    printf("║   Compiler: gcc -O3 -march=native (AVX2 autoVec)     ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    /* ── Part 1: Pure execution (no disk I/O) ── */
    print_section("Part 1 — Pure execution (in-memory, no disk I/O)");
    printf("  Measures: vec filter/agg/sort vs row model (Document** + qsort)\n");
    printf("  No disk I/O — isolates execution engine speed only.\n\n");
    print_row("", 0, 0, 0, 1); /* header */
    printf("  %-35s %9s  %9s  %10s\n",
           "---", "---", "---", "---");

    for (int si = 0; si < N_SIZES; si++) {
        int N = SIZES[si];

        /* Build in-memory docs + ColBatch */
        Document **docs = (Document**)malloc(N * sizeof(Document*));
        for (int i = 0; i < N; i++) docs[i] = make_doc(i);

        Arena *batch_arena = arena_new();
        const char *fields[] = {"score", "age", "region"};
        ColType     types[]  = {COL_TYPE_NUM, COL_TYPE_NUM, COL_TYPE_STR};
        ColBatch *b = col_batch_new(batch_arena, fields, types, 3, N);
        for (int i = 0; i < N; i++) col_batch_add_doc(b, docs[i], i);
        col_batch_finalize(b, N);

        printf("\n  [N = %d]\n", N);

        for (int q = 0; q < 4; q++) {
            PureCtx  vctx = { b, NULL, q, 500.0, 0 };
            PureRowCtx rctx = { docs, N, q, 500.0, 0 };

            MeasResult vr = measure(bench_pure_vec, &vctx);
            MeasResult rr = measure(bench_pure_row, &rctx);

            char lbl[80];
            snprintf(lbl, sizeof(lbl), "vec  %-28s", query_name(q));
            print_row(lbl, vr.avg_ms, rr.avg_ms, N, 0);
            snprintf(lbl, sizeof(lbl), "row  %-28s", query_name(q));
            print_row(lbl, rr.avg_ms, rr.avg_ms, N, 0);
        }

        for (int i = 0; i < N; i++) doc_free(docs[i]);
        free(docs);
        arena_free(batch_arena);
    }

    /* ── Part 2: Full pipeline (with disk I/O) ── */
    print_section("Part 2 — Full pipeline (DiskDB → vec_exec vs phys_exec)");
    printf("  Measures: end-to-end query latency including disk scan.\n");
    printf("  cold = first query after DB open (no cache)\n");
    printf("  warm = repeated query (scan cache hit)\n\n");
    print_row("", 0, 0, 0, 1);
    printf("  %-35s %9s  %9s  %10s\n",
           "---", "---", "---", "---");

    for (int si = 0; si < N_DISK_SIZES; si++) {
        int N = DISK_SIZES[si];

        printf("\n  [N = %d]  (inserting docs...)\n", N);
        DiskDB db;
        setup_db(&db, N);

        /* Get baseline: phys_exec (always reads from disk) */
        DiskCtx phys_ctx = { &db, 0, 500.0, 0, 0, 0 };
        MeasResult phys_r = measure(bench_disk, &phys_ctx);
        double base_ms = phys_r.avg_ms;

        for (int q = 0; q < 4; q++) {
            /* phys_exec (baseline) */
            DiskCtx pctx = { &db, q, 500.0, 0, 0, 0 };
            MeasResult pr = measure(bench_disk, &pctx);

            /* vec_exec cold (bulk scan, cache miss each run) */
            DiskCtx vcold = { &db, q, 500.0, 1, 1, 0 };
            MeasResult cr = measure(bench_disk, &vcold);

            /* vec_exec warm (cache hit) */
            DiskCtx vwarm = { &db, q, 500.0, 1, 0, 0 };
            /* pre-warm: run once to fill cache */
            scan_cache_invalidate(vec_get_scan_cache(), "bench");
            bench_disk(&vwarm);
            MeasResult wr = measure(bench_disk, &vwarm);

            char lbl[80];
            snprintf(lbl, sizeof(lbl), "phys %-28s", query_name(q));
            print_row(lbl, pr.avg_ms, pr.avg_ms, N, 0);
            snprintf(lbl, sizeof(lbl), "vec  cold %-24s", query_name(q));
            print_row(lbl, cr.avg_ms, pr.avg_ms, N, 0);
            snprintf(lbl, sizeof(lbl), "vec  warm %-24s", query_name(q));
            print_row(lbl, wr.avg_ms, pr.avg_ms, N, 0);
        }
        (void)base_ms;
        teardown_db(&db);
    }

    /* ── Part 3: Summary table ── */
    print_section("Part 3 — Summary: speedup ratios");
    printf("  Best-case speedup (warm cache) vs phys_exec baseline.\n\n");
    printf("  %-20s  %8s  %8s  %8s\n", "Query", "N=5k", "N=20k", "N=50k");
    printf("  %-20s  %8s  %8s  %8s\n", "--------------------",
           "--------","--------","--------");

    /* Re-run just filter query across all sizes for summary */
    double phys_ms[N_DISK_SIZES], warm_ms[N_DISK_SIZES];
    for (int si = 0; si < N_DISK_SIZES; si++) {
        int N = DISK_SIZES[si];
        DiskDB db; setup_db(&db, N);

        DiskCtx pctx = { &db, 0, 500.0, 0, 0, 0 };
        phys_ms[si] = measure(bench_disk, &pctx).avg_ms;

        /* warm */
        DiskCtx vwarm = { &db, 0, 500.0, 1, 0, 0 };
        scan_cache_invalidate(vec_get_scan_cache(), "bench");
        bench_disk(&vwarm);
        warm_ms[si] = measure(bench_disk, &vwarm).avg_ms;

        teardown_db(&db);
    }
    printf("  %-20s  %7.1fx  %7.1fx  %7.1fx\n",
           "filter (warm)",
           phys_ms[0]/warm_ms[0],
           phys_ms[1]/warm_ms[1],
           phys_ms[2]/warm_ms[2]);

    /* Pure vec filter summary */
    double pvec_ms[N_DISK_SIZES], prow_ms[N_DISK_SIZES];
    for (int si = 0; si < N_DISK_SIZES; si++) {
        int N = DISK_SIZES[si];
        Document **docs = (Document**)malloc(N * sizeof(Document*));
        for (int i = 0; i < N; i++) docs[i] = make_doc(i);

        Arena *ba = arena_new();
        const char *f[] = {"score","age","region"};
        ColType t[] = {COL_TYPE_NUM,COL_TYPE_NUM,COL_TYPE_STR};
        ColBatch *b = col_batch_new(ba,f,t,3,N);
        for (int i=0;i<N;i++) col_batch_add_doc(b,docs[i],i);
        col_batch_finalize(b,N);

        PureCtx  vc = {b,NULL,0,500.0,0};
        PureRowCtx rc = {docs,N,0,500.0,0};
        pvec_ms[si] = measure(bench_pure_vec,&vc).avg_ms;
        prow_ms[si] = measure(bench_pure_row,&rc).avg_ms;

        for(int i=0;i<N;i++) doc_free(docs[i]);
        free(docs); arena_free(ba);
    }
    printf("  %-20s  %7.1fx  %7.1fx  %7.1fx\n",
           "pure vec filter",
           prow_ms[0]/pvec_ms[0],
           prow_ms[1]/pvec_ms[1],
           prow_ms[2]/pvec_ms[2]);

    /* Pure vec sort summary */
    for (int si = 0; si < N_DISK_SIZES; si++) {
        int N = DISK_SIZES[si];
        Document **docs = (Document**)malloc(N * sizeof(Document*));
        for (int i = 0; i < N; i++) docs[i] = make_doc(i);

        Arena *ba = arena_new();
        const char *f[] = {"score","age","region"};
        ColType t[] = {COL_TYPE_NUM,COL_TYPE_NUM,COL_TYPE_STR};
        ColBatch *b = col_batch_new(ba,f,t,3,N);
        for (int i=0;i<N;i++) col_batch_add_doc(b,docs[i],i);
        col_batch_finalize(b,N);

        PureCtx  vc = {b,NULL,1,500.0,0};
        PureRowCtx rc = {docs,N,1,500.0,0};
        pvec_ms[si] = measure(bench_pure_vec,&vc).avg_ms;
        prow_ms[si] = measure(bench_pure_row,&rc).avg_ms;

        for(int i=0;i<N;i++) doc_free(docs[i]);
        free(docs); arena_free(ba);
    }
    printf("  %-20s  %7.1fx  %7.1fx  %7.1fx\n",
           "pure vec sort",
           prow_ms[0]/pvec_ms[0],
           prow_ms[1]/pvec_ms[1],
           prow_ms[2]/pvec_ms[2]);

    printf("\n  Legend:\n");
    printf("  phys = phys_exec (row model, disk read per query)\n");
    printf("  vec cold = vec_exec, bulk pread, no cache\n");
    printf("  vec warm = vec_exec, scan cache hit (zero disk I/O)\n");
    printf("  pure = in-memory only, no DiskDB\n");
    printf("\n");

    return 0;
}
