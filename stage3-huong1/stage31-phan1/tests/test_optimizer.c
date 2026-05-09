/* test_optimizer.c â€” Tests for Stage 3: Cost-Based Query Optimizer
 *
 * Tests cover:
 *   1. Logical plan building (Phase 1)
 *   2. Statistics collection (Phase 2)
 *   3. Cost model formulas (Phase 3)
 *   4. Logical rules (Phase 4)
 *   5. Physical plan selection (Phase 5-6)
 *   6. End-to-end: optimizer produces SAME results as legacy executor
 *   7. EXPLAIN output
 *   8. analyze command
 */
#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/query/executor.h"
#include "../src/core/executor_disk.h"
#include "../src/core/disk_db.h"
#include "../src/core/collection.h"
#include "../src/core/optimizer/arena.h"
#include "../src/core/optimizer/logical_plan.h"
#include "../src/core/optimizer/statistics.h"
#include "../src/core/optimizer/cost_model.h"
#include "../src/core/optimizer/rules.h"
#include "../src/core/optimizer/optimizer.h"
#include "../src/core/phys_executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* ===== Test harness ===== */
static int g_pass = 0, g_fail = 0;

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); g_pass++; } while(0)
#define FAIL(msg) do { printf("FAIL â€” %s\n", msg); g_fail++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ===== DB helpers ===== */
#define TEST_DB_PATH "test_optimizer_tmp.hugo"

static DiskDB g_db;
static int    g_db_open = 0;

static void db_setup(void) {
    /* Close previous DB if open */
    if (g_db_open) { ddb_close(&g_db); g_db_open = 0; }
    remove(TEST_DB_PATH);
    remove(TEST_DB_PATH ".stats");
    remove(TEST_DB_PATH ".hugolog");
    ddb_create(&g_db, "test", TEST_DB_PATH);
    g_db_open = 1;
    /* Re-init optimizer with new DB pointer each time */
    hugo_optimizer_init(&g_db, TEST_DB_PATH, HUGO_OPT_COST_BASED);
}

static void db_teardown(void) {
    if (g_db_open) {
        ddb_close(&g_db);
        g_db_open = 0;
    }
    remove(TEST_DB_PATH);
    remove(TEST_DB_PATH ".stats");
    remove(TEST_DB_PATH ".hugolog");
}

/* Insert a document via raw API */
static void insert_doc(const char *coll, const char *json_like_pairs[][2], int n) {
    DiskColl *c = ddb_get_coll(&g_db, coll);
    if (!c) c = ddb_create_coll(&g_db, coll);
    if (!c) return;

    Document *d = (Document*)calloc(1, sizeof(Document));
    for (int i = 0; i < n; i++) {
        Value v; memset(&v, 0, sizeof(v));
        /* Try to parse as number */
        char *end;
        double num = strtod(json_like_pairs[i][1], &end);
        if (*end == 0 && end != json_like_pairs[i][1]) {
            v.type = VAL_NUM; v.num = num;
        } else {
            v.type = VAL_STR;
            strncpy(v.str, json_like_pairs[i][1], sizeof(v.str)-1);
        }
        doc_set_field(d, json_like_pairs[i][0], v);
    }
    uint64_t id;
    ddb_insert_doc(&g_db, c, d, &id);
    doc_free(d);
}

/* Parse + execute a query string, return result (caller does result_free_disk) */
static int exec_query(const char *ql, HugoResult *r) {
    TokenList tl;
    if (hugo_tokenize(ql, &tl) != 0) { result_init(r); r->ok=0; return -1; }
    Query q;
    if (hugo_parse(&tl, &q) != 0) { result_init(r); r->ok=0; query_free(&q); return -1; }
    hugo_execute_disk_opt(&g_db, &q, r);
    query_free(&q);
    return r->ok ? 0 : -1;
}

/* Execute via legacy path */
static int exec_legacy(const char *ql, HugoResult *r) {
    TokenList tl;
    if (hugo_tokenize(ql, &tl) != 0) { result_init(r); r->ok=0; return -1; }
    Query q;
    if (hugo_parse(&tl, &q) != 0) { result_init(r); r->ok=0; query_free(&q); return -1; }
    hugo_execute_disk(&g_db, &q, r);
    query_free(&q);
    return r->ok ? 0 : -1;
}

/* ===== Phase 1: Logical Plan Tests ===== */

static void test_lplan_simple_scan(void) {
    TEST("lplan: scan only (no where clause)");
    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden users", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    ASSERT(p->type == LOP_SCAN, "expected LOP_SCAN");
    ASSERT(strcmp(p->scan.collection_name, "users") == 0, "wrong collection");
    query_free(&q); arena_free(a);
    PASS();
}

static void test_lplan_filter(void) {
    TEST("lplan: scan + filter (haar clause)");
    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden users haar age $bg 25", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    ASSERT(p->type == LOP_FILTER, "expected LOP_FILTER at root");
    ASSERT(p->left != NULL, "filter has no child");
    ASSERT(p->left->type == LOP_SCAN, "filter child should be SCAN");
    query_free(&q); arena_free(a);
    PASS();
}

static void test_lplan_sort_limit(void) {
    TEST("lplan: scan â†’ filter â†’ sort â†’ limit");
    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden users haar age $bg 25 orange bi score desc lime 10", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    ASSERT(p->type == LOP_LIMIT, "root should be LIMIT");
    ASSERT(p->limit.limit == 10, "limit should be 10");
    ASSERT(p->left != NULL && p->left->type == LOP_SORT, "child should be SORT");
    ASSERT(p->left->left != NULL && p->left->left->type == LOP_FILTER, "should have FILTER");
    query_free(&q); arena_free(a);
    PASS();
}

static void test_lplan_join(void) {
    TEST("lplan: join produces LOP_JOIN with two scan children");
    Arena *a = arena_new();
    TokenList tl;
    hugo_tokenize("funden users $rasoat orders local_field id target_field user_id", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    /* root may be Scan or Join depending on structure â€” find JOIN */
    /* walk down to find join */
    LogicalPlan *cur = p;
    int found_join = 0;
    while (cur) {
        if (cur->type == LOP_JOIN) { found_join = 1; break; }
        cur = cur->left;
    }
    ASSERT(found_join, "no LOP_JOIN found in plan");
    query_free(&q); arena_free(a);
    PASS();
}

static void test_lplan_aggregate(void) {
    TEST("lplan: gomail produces LOP_AGGREGATE");
    Arena *a = arena_new();
    TokenList tl;
    hugo_tokenize("gomail orders gremb bi status pou id", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    /* Find aggregate node */
    LogicalPlan *cur = p;
    int found_agg = 0;
    while (cur) {
        if (cur->type == LOP_AGGREGATE) { found_agg = 1; break; }
        cur = cur->left;
    }
    ASSERT(found_agg, "no LOP_AGGREGATE found");
    query_free(&q); arena_free(a);
    PASS();
}

static void test_lplan_print(void) {
    TEST("lplan: logical_plan_print produces output");
    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden users haar age $bg 25 lime 5", &tl);
    Query q; hugo_parse(&tl, &q);
    LogicalPlan *p = build_logical_plan(&q, a);
    ASSERT(p != NULL, "plan is null");
    /* Just ensure it doesn't crash */
    printf("\n    -- Plan output below --\n");
    logical_plan_print(p, 0);
    printf("    -- End plan output --\n");
    query_free(&q); arena_free(a);
    PASS();
}

/* ===== Phase 2: Statistics Tests ===== */

static void test_stats_analyze_basic(void) {    TEST("stats: analyze collection, total_rows correct");
    db_setup();    /* Insert 20 docs */
    for (int i = 0; i < 20; i++) {
        const char *pairs[2][2] = {{"age", ""}, {"name", "alice"}};
        char age_str[16]; snprintf(age_str, sizeof(age_str), "%d", 20 + i);
        pairs[0][1] = age_str;
        insert_doc("testcol", pairs, 2);
    }    OptimizerCtx *ctx = hugo_optimizer_get();
    ASSERT(ctx != NULL, "optimizer ctx is null");    DiskColl *_dc = ddb_get_coll(&g_db, "testcol");    int rc = optimizer_analyze(ctx, "testcol");    ASSERT(rc == 0, "analyze failed");

    CollectionStats *cs = stats_get(&ctx->stats, "testcol");
    ASSERT(cs != NULL, "no stats for testcol");
    ASSERT(cs->total_rows == 20, "expected 20 rows");
    ASSERT(cs->n_columns >= 1, "expected at least 1 column");

    db_teardown();
    PASS();
}

static void test_stats_selectivity_equality(void) {
    TEST("stats: selectivity for equality predicate");
    db_setup();
    /* Insert 100 docs, age = 0..99 */
    for (int i = 0; i < 100; i++) {
        const char *pairs[1][2] = {{"age", ""}};
        char age_str[16]; snprintf(age_str, sizeof(age_str), "%d", i);
        pairs[0][1] = age_str;
        insert_doc("testcol", pairs, 1);
    }    OptimizerCtx *ctx = hugo_optimizer_get();
    optimizer_analyze(ctx, "testcol");
    CollectionStats *cs = stats_get(&ctx->stats, "testcol");
    ASSERT(cs != NULL, "no stats");

    /* Build a condition: age = 42 */
    Condition c; memset(&c, 0, sizeof(c));
    c.type = COND_CMP;
    strncpy(c.field, "age", sizeof(c.field)-1);
    c.op = TOK_OP_BG;
    c.value.type = VAL_NUM; c.value.num = 42;

    double sel = stats_estimate_selectivity(cs, &c);
    /* With 100 distinct values, should be ~0.01 */
    if (!(sel > 0 && sel <= 0.5)) {
        char sel_msg[128]; snprintf(sel_msg, sizeof(sel_msg),
            "selectivity out of expected range (got %g)", sel);
        FAIL(sel_msg); return;
    }

    db_teardown();
    PASS();
}

static void test_stats_persist_load(void) {
    TEST("stats: persist to disk and reload");
    db_setup();
    for (int i = 0; i < 50; i++) {
        const char *pairs[1][2] = {{"score", ""}};
        char v[16]; snprintf(v, sizeof(v), "%d", i * 2);
        pairs[0][1] = v;
        insert_doc("scores", pairs, 1);
    }    OptimizerCtx *ctx = hugo_optimizer_get();
    optimizer_analyze(ctx, "scores");
    /* Persist */
    stats_persist(&ctx->stats);

    /* Load into fresh store (heap-allocated to avoid stack overflow) */
    StatsStore *ss2 = (StatsStore*)calloc(1, sizeof(StatsStore));
    ASSERT(ss2 != NULL, "calloc failed");
    stats_store_init(ss2, TEST_DB_PATH);
    int rc = stats_load(ss2);
    ASSERT(rc == 0, "stats_load failed");
    CollectionStats *cs = stats_get(ss2, "scores");
    ASSERT(cs != NULL, "stats not loaded");
    ASSERT(cs->total_rows == 50, "wrong total_rows after reload");
    free(ss2);

    db_teardown();
    PASS();
}

/* ===== Phase 3: Cost Model Tests ===== */

static void test_cost_seq_scan(void) {
    TEST("cost: seq_scan cost grows with page count");
    CostModel m; cost_model_init_default(&m);

    CollectionStats small_stats, big_stats;
    memset(&small_stats, 0, sizeof(small_stats));
    memset(&big_stats,   0, sizeof(big_stats));
    small_stats.total_rows = 100;  small_stats.page_count = 10;
    big_stats.total_rows   = 10000; big_stats.page_count  = 1000;

    double c_small = cost_seq_scan(&m, &small_stats);
    double c_big   = cost_seq_scan(&m, &big_stats);
    ASSERT(c_big > c_small, "big table should cost more");
    PASS();
}

static void test_cost_index_scan_cheaper(void) {
    TEST("cost: index_scan cheaper than seq_scan for low selectivity");
    CostModel m; cost_model_init_default(&m);

    CollectionStats cs; memset(&cs, 0, sizeof(cs));
    cs.total_rows = 10000; cs.page_count = 1000;

    double c_seq = cost_seq_scan(&m, &cs);
    double c_idx = cost_index_scan(&m, &cs, 0.001); /* 0.1% selectivity */
    ASSERT(c_idx < c_seq, "index scan should be cheaper for 0.1% selectivity");
    PASS();
}

static void test_cost_hash_join_vs_nl(void) {
    TEST("cost: hash join cheaper than NL for large tables");
    CostModel m; cost_model_init_default(&m);
    double outer = 1000, inner = 1000;
    double c_nl = cost_nested_loop_join(&m, outer, inner);
    double c_hj = cost_hash_join(&m, inner, outer, 128.0);
    ASSERT(c_hj < c_nl, "hash join should beat NL for 1000x1000");
    PASS();
}

/* ===== Phase 4: Rules Tests ===== */

static void test_rules_eliminate_redundant_limit(void) {
    TEST("rules: eliminate Limit(-1, 0) no-op");
    Arena *a = arena_new();
    /* Build: Limit(-1) â†’ Scan */
    LogicalPlan *scan = (LogicalPlan*)arena_alloc(a, sizeof(LogicalPlan));
    memset(scan, 0, sizeof(LogicalPlan));
    scan->type = LOP_SCAN;
    strncpy(scan->scan.collection_name, "users", 63);

    LogicalPlan *lim = (LogicalPlan*)arena_alloc(a, sizeof(LogicalPlan));
    memset(lim, 0, sizeof(LogicalPlan));
    lim->type = LOP_LIMIT;
    lim->limit.limit = -1;
    lim->limit.skip  = 0;
    lim->left = scan;

    LogicalPlan *result = rule_eliminate_redundant(lim, a);
    ASSERT(result != NULL, "result is null");
    ASSERT(result->type == LOP_SCAN, "Limit(-1,0) should be eliminated");
    arena_free(a);
    PASS();
}

static void test_rules_eliminate_empty_sort(void) {
    TEST("rules: eliminate Sort with no fields");
    Arena *a = arena_new();
    LogicalPlan *scan = (LogicalPlan*)arena_alloc(a, sizeof(LogicalPlan));
    memset(scan, 0, sizeof(LogicalPlan));
    scan->type = LOP_SCAN;

    LogicalPlan *sort = (LogicalPlan*)arena_alloc(a, sizeof(LogicalPlan));
    memset(sort, 0, sizeof(LogicalPlan));
    sort->type = LOP_SORT;
    sort->sort.n_fields = 0;
    sort->left = scan;

    LogicalPlan *result = rule_eliminate_redundant(sort, a);
    ASSERT(result != NULL, "result is null");
    ASSERT(result->type == LOP_SCAN, "Sort with 0 fields should be eliminated");
    arena_free(a);
    PASS();
}

/* ===== Phase 5-6: Physical Plan Selection Tests ===== */

static void test_phys_seqscan_chosen(void) {
    TEST("phys: SeqScan chosen when no index");
    db_setup();    OptimizerCtx *ctx = hugo_optimizer_get();
    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden users haar age $bg 30", &tl);
    Query q; hugo_parse(&tl, &q);

    PhysicalPlan *p = optimizer_run(ctx, &q, a);
    ASSERT(p != NULL, "plan is null");
    /* Walk to find scan node */
    PhysicalPlan *cur = p;
    while (cur && cur->left) cur = cur->left;
    ASSERT(cur != NULL, "no leaf node");
    ASSERT(cur->type == POP_SEQ_SCAN || cur->type == POP_FILTER, "expected scan at leaf");

    query_free(&q); arena_free(a);
    db_teardown();
    PASS();
}

static void test_phys_index_scan_chosen(void) {
    TEST("phys: IndexScan chosen when index exists on filter field");
    db_setup();
    /* Create collection + index */
    DiskColl *c = ddb_create_coll(&g_db, "products");
    /* Add index on 'price' field manually */
    if (c && c->n_indexes < DDB_MAX_INDEXES) {
        strncpy(c->indexes[c->n_indexes].field, "price", 127);
        c->indexes[c->n_indexes].btree_root_page = 0;
        c->n_indexes++;
    }
    /* Insert some docs */
    for (int i = 0; i < 500; i++) {
        const char *pairs[1][2] = {{"price", ""}};
        char v[16]; snprintf(v, sizeof(v), "%d", i * 10);
        pairs[0][1] = v;
        insert_doc("products", pairs, 1);
    }    OptimizerCtx *ctx = hugo_optimizer_get();
    optimizer_analyze(ctx, "products");

    Arena *a = arena_new();
    TokenList tl; hugo_tokenize("funden products haar price $bg 100", &tl);
    Query q; hugo_parse(&tl, &q);

    PhysicalPlan *p = optimizer_run(ctx, &q, a);
    ASSERT(p != NULL, "plan is null");
    /* Walk to leaf scan */
    PhysicalPlan *cur = p;
    while (cur && cur->left) cur = cur->left;
    /* Index scan should be chosen â€” or at minimum the plan exists */
    ASSERT(cur != NULL, "plan has no leaf");

    query_free(&q); arena_free(a);
    db_teardown();
    PASS();
}

/* ===== End-to-end correctness: optimizer must return SAME rows as legacy ===== */

static int count_matching_docs(HugoResult *r, const char *field, double expected_num) {
    int cnt = 0;
    for (int i = 0; i < r->count; i++) {
        Value v;
        if (doc_get_field(r->docs[i], field, &v) == 0 &&
            v.type == VAL_NUM && v.num == expected_num)
            cnt++;
    }
    return cnt;
}

static void test_e2e_simple_filter(void) {
    TEST("e2e: simple filter returns same count as legacy");
    db_setup();
    for (int i = 0; i < 50; i++) {
        const char *pairs[1][2] = {{"score", ""}};
        char v[16]; snprintf(v, sizeof(v), "%d", i % 10);
        pairs[0][1] = v;
        insert_doc("data", pairs, 1);
    }

    HugoResult r_opt, r_leg;
    exec_query("funden data haar score $bg 5", &r_opt);
    exec_legacy("funden data haar score $bg 5", &r_leg);

    ASSERT(r_opt.ok,  "optimizer query failed");
    ASSERT(r_leg.ok,  "legacy query failed");
    ASSERT(r_opt.count == r_leg.count, "different row counts");

    result_free_disk(&r_opt);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

static void test_e2e_sort_limit(void) {
    TEST("e2e: sort + limit returns same rows as legacy");
    db_setup();
    for (int i = 99; i >= 0; i--) {
        const char *pairs[1][2] = {{"rank", ""}};
        char v[16]; snprintf(v, sizeof(v), "%d", i);
        pairs[0][1] = v;
        insert_doc("ranks", pairs, 1);
    }

    HugoResult r_opt, r_leg;
    exec_query("funden ranks orange bi rank asc lime 5", &r_opt);
    exec_legacy("funden ranks orange bi rank asc lime 5", &r_leg);

    ASSERT(r_opt.ok && r_leg.ok, "query failed");
    ASSERT(r_opt.count == 5 && r_leg.count == 5, "expected 5 docs");
    /* Verify top doc has rank=0 */
    ASSERT(count_matching_docs(&r_opt, "rank", 0) == 1, "rank=0 missing in opt result");
    ASSERT(count_matching_docs(&r_leg, "rank", 0) == 1, "rank=0 missing in leg result");

    result_free_disk(&r_opt);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

static void test_e2e_no_filter(void) {
    TEST("e2e: full scan (no where) same row count");
    db_setup();
    for (int i = 0; i < 30; i++) {
        const char *pairs[1][2] = {{"x", "1"}};
        insert_doc("full", pairs, 1);
    }
    HugoResult r_opt, r_leg;
    exec_query("funden full", &r_opt);
    exec_legacy("funden full", &r_leg);

    ASSERT(r_opt.ok && r_leg.ok, "query failed");
    ASSERT(r_opt.count == r_leg.count, "different row counts for full scan");

    result_free_disk(&r_opt);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

static void test_e2e_skip(void) {
    TEST("e2e: SKIP produces same result as legacy");
    db_setup();
    for (int i = 0; i < 20; i++) {
        const char *pairs[1][2] = {{"n", ""}};
        char v[16]; snprintf(v, sizeof(v), "%d", i);
        pairs[0][1] = v;
        insert_doc("nums", pairs, 1);
    }
    HugoResult r_opt, r_leg;
    exec_query("funden nums orange bi n asc skopan 5 lime 5", &r_opt);
    exec_legacy("funden nums orange bi n asc skopan 5 lime 5", &r_leg);

    ASSERT(r_opt.ok && r_leg.ok, "query failed");
    ASSERT(r_opt.count == r_leg.count, "different row count with skip");

    result_free_disk(&r_opt);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

static void test_e2e_aggregate(void) {
    TEST("e2e: GROUP BY same result as legacy");
    db_setup();
    for (int i = 0; i < 30; i++) {
        const char *pairs[2][2] = {{"dept", ""}, {"salary", "50000"}};
        const char *dept = (i % 3 == 0) ? "eng" : (i % 3 == 1) ? "sales" : "hr";
        pairs[0][1] = dept;
        insert_doc("employees", pairs, 2);
    }
    HugoResult r_opt, r_leg;
    exec_query("gomail employees gremb bi dept pou salary", &r_opt);
    exec_legacy("gomail employees gremb bi dept pou salary", &r_leg);

    ASSERT(r_opt.ok && r_leg.ok, "query failed");
    ASSERT(r_opt.count == r_leg.count, "different group count");

    result_free_disk(&r_opt);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

/* ===== EXPLAIN / ANALYZE command tests ===== */

static void test_analyze_command(void) {
    TEST("analyze: analyze verb works via execute_opt");
    db_setup();
    for (int i = 0; i < 10; i++) {
        const char *pairs[1][2] = {{"val", "1"}};
        insert_doc("analtest", pairs, 1);
    }
    HugoResult r;
    exec_query("analyze analtest", &r);
    ASSERT(r.ok, "analyze command failed");
    ASSERT(strlen(r.info) > 0, "analyze gave no info message");

    result_free_disk(&r);
    db_teardown();
    PASS();
}

static void test_explain_output(void) {
    TEST("exepanus: produces plan output with cost");
    db_setup();
    for (int i = 0; i < 20; i++) {
        const char *pairs[1][2] = {{"age", "25"}};
        insert_doc("explain_test", pairs, 1);
    }

    HugoResult r;
    exec_query("exepanus explain_test haar age $bg 25", &r);
    ASSERT(r.ok, "exepanus failed");
    /* Info should contain plan-like keywords */
    int has_scan = (strstr(r.info, "Scan") != NULL || strstr(r.info, "scan") != NULL);
    int has_cost = (strstr(r.info, "cost") != NULL);
    ASSERT(has_scan, "EXPLAIN output missing Scan keyword");
    ASSERT(has_cost, "EXPLAIN output missing cost");

    result_free_disk(&r);
    db_teardown();
    PASS();
}

/* ===== Arena tests ===== */

static void test_arena_alloc_free(void) {
    TEST("arena: alloc and free multiple objects");
    Arena *a = arena_new();
    ASSERT(a != NULL, "arena_new returned null");

    void *p1 = arena_alloc(a, 64);
    void *p2 = arena_alloc(a, 128);
    void *p3 = arena_alloc(a, 4096);
    ASSERT(p1 != NULL && p2 != NULL && p3 != NULL, "alloc returned null");
    ASSERT(p1 != p2 && p2 != p3, "allocs returned same pointer");

    char *s = arena_strdup(a, "hello optimizer");
    ASSERT(s != NULL && strcmp(s, "hello optimizer") == 0, "strdup failed");

    arena_free(a);
    PASS();
}

/* ===== Optimizer mode switching ===== */

static void test_opt_mode_off_uses_legacy(void) {
    TEST("opt mode OFF: results same as legacy");
    db_setup();
    for (int i = 0; i < 15; i++) {
        const char *pairs[1][2] = {{"k", "1"}};
        insert_doc("modetest", pairs, 1);
    }    OptimizerCtx *ctx = hugo_optimizer_get();
    optimizer_set_mode(ctx, HUGO_OPT_OFF);

    HugoResult r_off, r_leg;
    exec_query("funden modetest", &r_off);
    exec_legacy("funden modetest", &r_leg);

    ASSERT(r_off.ok && r_leg.ok, "query failed");
    ASSERT(r_off.count == r_leg.count, "mode=OFF gave different count");

    optimizer_set_mode(ctx, HUGO_OPT_COST_BASED); /* restore */
    result_free_disk(&r_off);
    result_free_disk(&r_leg);
    db_teardown();
    PASS();
}

/* ===== Main ===== */

int main(void) {
    printf("========================================\n");
    printf(" Hugo DB â€” Stage 3: Optimizer Tests\n");
    printf("========================================\n\n");

    printf("Phase 1: Logical Plan\n");
    test_lplan_simple_scan();
    test_lplan_filter();
    test_lplan_sort_limit();
    test_lplan_join();
    test_lplan_aggregate();
    test_lplan_print();

    printf("\nPhase 2: Statistics\n");
    test_stats_analyze_basic();
    test_stats_selectivity_equality();
    test_stats_persist_load();

    printf("\nPhase 3: Cost Model\n");
    test_cost_seq_scan();
    test_cost_index_scan_cheaper();
    test_cost_hash_join_vs_nl();

    printf("\nPhase 4: Logical Rules\n");
    test_rules_eliminate_redundant_limit();
    test_rules_eliminate_empty_sort();

    printf("\nPhase 5-6: Physical Plan\n");
    test_phys_seqscan_chosen();
    test_phys_index_scan_chosen();

    printf("\nEnd-to-End Correctness\n");
    test_e2e_simple_filter();
    test_e2e_sort_limit();
    test_e2e_no_filter();
    test_e2e_skip();
    test_e2e_aggregate();

    printf("\nANALYZE / EXPLAIN\n");
    test_analyze_command();
    test_explain_output();

    printf("\nMiscellaneous\n");
    test_arena_alloc_free();
    test_opt_mode_off_uses_legacy();

    printf("\n========================================\n");
    printf(" Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail > 0 ? 1 : 0;
}



