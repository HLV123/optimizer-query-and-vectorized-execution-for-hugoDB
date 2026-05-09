/* test_phase10.c — Bulk import + Batch runner */
#include "../src/core/bulk_import.h"
#include "../src/core/batch_runner.h"
#include "../src/core/disk_db.h"
#include "../src/core/collection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* Create temporary JSONL file with N docs */
static void write_jsonl(const char *path, int n) {
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < n; i++) {
        fprintf(f, "{\"name\":\"user_%d\",\"age\":%d,\"active\":%s}\n",
                i, 18 + (i % 50), (i % 2) ? "true" : "false");
    }
    fclose(f);
}

/* ===== Test 1: import 100 docs from file ===== */
static void test_import_small(void) {
    printf("\n[1] Import 100 docs từ JSONL file\n");
    const char *db_path = "p10_small.hugo";
    const char *jsonl = "p10_small.jsonl";
    remove(db_path); remove("p10_small.hugolog");

    write_jsonl(jsonl, 100);

    DiskDB db;
    ddb_create(&db, "t", db_path);

    BulkStats stats;
    int rc = bulk_import_file(&db, "users", jsonl, &stats);
    CHECK(rc == 0, "import OK");
    CHECK(stats.lines_read == 100, "100 lines read");
    CHECK(stats.docs_inserted == 100, "100 docs inserted");
    CHECK(stats.parse_errors == 0, "no parse errors");

    DiskColl *c = ddb_get_coll(&db, "users");
    CHECK(c != NULL && c->count == 100, "collection has 100 docs");

    /* Verify 1 doc đọc đúng */
    Document *d = ddb_read_doc(&db, c, 1);
    CHECK(d != NULL, "read doc 1");
    Value v;
    CHECK(doc_get_field(d, "name", &v) == 0 && strcmp(v.str, "user_0") == 0,
          "first doc name = user_0");
    CHECK(doc_get_field(d, "age", &v) == 0 && v.num == 18, "age = 18");
    CHECK(doc_get_field(d, "active", &v) == 0 && v.type == VAL_BOOL,
          "active is bool");
    doc_free(d);

    ddb_close(&db);
    remove(db_path); remove("p10_small.hugolog"); remove(jsonl);
}

/* ===== Test 2: parse edge cases ===== */
static void test_parse_edges(void) {
    printf("\n[2] Parse edge cases: escape, unicode, comment lines\n");
    const char *db_path = "p10_edge.hugo";
    const char *jsonl = "p10_edge.jsonl";
    remove(db_path); remove("p10_edge.hugolog");

    FILE *f = fopen(jsonl, "wb");
    /* Dòng có escape */
    fprintf(f, "{\"name\":\"has \\\"quote\\\" inside\",\"path\":\"c:\\\\tmp\\\\x\"}\n");
    /* Dòng có unicode escape */
    fprintf(f, "{\"city\":\"H\\u00e0 N\\u1ed9i\"}\n");
    /* Dòng blank */
    fprintf(f, "\n");
    /* Comment */
    fprintf(f, "# a comment line\n");
    /* Nested object (skip field) */
    fprintf(f, "{\"name\":\"nested\",\"meta\":{\"x\":1,\"y\":2},\"age\":30}\n");
    /* Broken line */
    fprintf(f, "{this is not json}\n");
    /* Valid */
    fprintf(f, "{\"name\":\"last\"}\n");
    fclose(f);

    DiskDB db;
    ddb_create(&db, "t", db_path);
    BulkStats stats;
    bulk_import_file(&db, "edge", jsonl, &stats);

    CHECK(stats.docs_inserted == 4, "4 valid docs inserted");
    CHECK(stats.parse_errors == 1, "1 parse error (broken line)");

    /* Verify escape */
    DiskColl *c = ddb_get_coll(&db, "edge");
    Document *d1 = ddb_read_doc(&db, c, 1);
    Value v;
    CHECK(doc_get_field(d1, "name", &v) == 0 && strstr(v.str, "quote") != NULL,
          "escape string extracted");
    doc_free(d1);

    /* Verify unicode */
    Document *d2 = ddb_read_doc(&db, c, 2);
    CHECK(doc_get_field(d2, "city", &v) == 0 && strstr(v.str, "Hà") != NULL,
          "unicode escape → UTF-8 Hà");
    doc_free(d2);

    /* Verify nested skipped but other fields present */
    Document *d3 = ddb_read_doc(&db, c, 3);
    CHECK(doc_get_field(d3, "name", &v) == 0 && strcmp(v.str, "nested") == 0,
          "nested: name extracted");
    CHECK(doc_get_field(d3, "age", &v) == 0 && v.num == 30,
          "nested: age extracted");
    CHECK(doc_get_field(d3, "meta", &v) != 0, "nested: meta field skipped");
    doc_free(d3);

    ddb_close(&db);
    remove(db_path); remove("p10_edge.hugolog"); remove(jsonl);
}

/* ===== Test 3: large import — 10,000 docs ===== */
static void test_import_large(void) {
    printf("\n[3] Large import: 10,000 docs + throughput measure\n");
    const char *db_path = "p10_large.hugo";
    const char *jsonl = "p10_large.jsonl";
    remove(db_path); remove("p10_large.hugolog");

    write_jsonl(jsonl, 10000);

    DiskDB db;
    ddb_create(&db, "t", db_path);
    BulkStats stats;
    int rc = bulk_import_file(&db, "users", jsonl, &stats);
    CHECK(rc == 0, "import 10k OK");
    CHECK(stats.docs_inserted == 10000, "10,000 docs inserted");

    double docs_per_sec = stats.elapsed_sec > 0 ?
        stats.docs_inserted / stats.elapsed_sec : 0;
    printf("       imported %llu docs in %.3fs (%.0f docs/sec, %.1f KB/s)\n",
           (unsigned long long)stats.docs_inserted, stats.elapsed_sec,
           docs_per_sec, stats.bytes_read / 1024.0 / (stats.elapsed_sec > 0 ? stats.elapsed_sec : 1));

    /* Persistence check */
    ddb_close(&db);
    DiskDB db2;
    ddb_open(&db2, db_path);
    DiskColl *c = ddb_get_coll(&db2, "users");
    CHECK(c != NULL && c->count == 10000, "10k docs persistent after reopen");
    ddb_close(&db2);

    remove(db_path); remove("p10_large.hugolog"); remove(jsonl);
}

/* ===== Test 4: batch runner ===== */
static void test_batch_runner(void) {
    printf("\n[4] Batch runner: chạy .hugoql file + timing\n");
    const char *db_path = "p10_batch.hugo";
    const char *qfile = "p10_batch.hugoql";
    remove(db_path); remove("p10_batch.hugolog");

    /* Setup DB */
    DiskDB db;
    ddb_create(&db, "t", db_path);

    /* Query file */
    FILE *f = fopen(qfile, "wb");
    fprintf(f, "-- setup\n");
    fprintf(f, "madeco users\n");
    fprintf(f, "vietinfo users { name: \"Alice\", age: 20 }\n");
    fprintf(f, "vietinfo users { name: \"Bob\", age: 22 }\n");
    fprintf(f, "vietinfo users { name: \"Carol\", age: 25 }\n");
    fprintf(f, "# ^ a comment line too\n");
    fprintf(f, "\n"); /* blank */
    fprintf(f, "funden users\n");
    fprintf(f, "funden users haar age $bh 21\n");
    fprintf(f, "cochin users haar name $bg \"Alice\" $quy age 21\n");
    fprintf(f, "funden users haar name $bg \"Alice\"\n");
    fprintf(f, "funden nope\n");  /* error intentional */
    fclose(f);

    BatchStats stats;
    int rc = batch_run_file(&db, qfile, 0, NULL, &stats);
    CHECK(rc == 0, "batch run OK");
    CHECK(stats.queries_total == 9, "9 queries (comments/blanks skipped)");
    CHECK(stats.queries_ok == 8, "8 OK (1 error for funden nope)");
    CHECK(stats.queries_err == 1, "1 error");
    CHECK(stats.avg_ms >= 0, "avg ms reported");

    printf("       9 queries: total=%.3fs avg=%.3fms p50=%.3fms p95=%.3fms p99=%.3fms\n",
           stats.total_sec, stats.avg_ms, stats.p50_ms, stats.p95_ms, stats.p99_ms);

    ddb_close(&db);
    remove(db_path); remove("p10_batch.hugolog"); remove(qfile);
}

/* ===== Test 5: batch runner from buffer (HTTP path) ===== */
static void test_batch_buffer(void) {
    printf("\n[5] Batch runner qua buffer (cho HTTP /batch)\n");
    const char *db_path = "p10_bbuf.hugo";
    remove(db_path); remove("p10_bbuf.hugolog");

    DiskDB db;
    ddb_create(&db, "t", db_path);

    const char *batch =
        "madeco orders\n"
        "vietinfo orders { user: \"Alice\", total: 100 }\n"
        "vietinfo orders { user: \"Bob\", total: 250 }\n"
        "funden orders\n"
        "funden orders haar total $bh 150\n";

    BatchStats stats;
    char *json_out = NULL;
    int rc = batch_run_buffer(&db, batch, strlen(batch), 0, NULL,
                              &stats, &json_out);
    CHECK(rc == 0, "batch_run_buffer OK");
    CHECK(stats.queries_total == 5, "5 queries");
    CHECK(stats.queries_ok == 5, "all OK");
    CHECK(json_out != NULL, "json output generated");
    CHECK(strstr(json_out, "\"query\":") != NULL, "json has query field");
    CHECK(strstr(json_out, "\"ms\":") != NULL, "json has ms field");
    CHECK(strstr(json_out, "\"result\":") != NULL, "json has result field");
    CHECK(strstr(json_out, "Alice") != NULL, "Alice in some result");
    free(json_out);

    ddb_close(&db);
    remove(db_path); remove("p10_bbuf.hugolog");
}

/* ===== Test 6: import + query realistic flow ===== */
static void test_import_and_query(void) {
    printf("\n[6] E2E: import 1000 docs + batch query với filter\n");
    const char *db_path = "p10_flow.hugo";
    const char *jsonl = "p10_flow.jsonl";
    remove(db_path); remove("p10_flow.hugolog");

    write_jsonl(jsonl, 1000);

    DiskDB db;
    ddb_create(&db, "t", db_path);
    BulkStats bs;
    bulk_import_file(&db, "users", jsonl, &bs);
    CHECK(bs.docs_inserted == 1000, "imported 1000");

    /* Mix queries */
    const char *batch =
        "funden users lime 5\n"
        "funden users haar age $bh 40\n"
        "funden users haar active $bg true lime 10\n"
        "funden users haar age $lhb 20\n"
        "skill users\n";
    BatchStats bts;
    char *json_out = NULL;
    batch_run_buffer(&db, batch, strlen(batch), 0, NULL, &bts, &json_out);
    CHECK(bts.queries_total == 5, "5 batch queries");
    CHECK(bts.queries_ok == 5, "all OK on 1000 docs");
    printf("       5 queries on 1000-row DB: avg=%.3fms max=%.3fms\n",
           bts.avg_ms, bts.max_ms);
    free(json_out);

    ddb_close(&db);
    remove(db_path); remove("p10_flow.hugolog"); remove(jsonl);
}

int main(void) {
    printf("=== HUGO DB — Phase 10 (Bulk Import + Batch Runner) Tests ===\n");
    test_import_small();
    test_parse_edges();
    test_import_large();
    test_batch_runner();
    test_batch_buffer();
    test_import_and_query();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
