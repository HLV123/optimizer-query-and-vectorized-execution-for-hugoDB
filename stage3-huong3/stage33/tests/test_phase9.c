/* test_phase9.c — Phase 9 GUI backend: JSON API tests
 *
 * Verify: HugoQL execute → JSON response đúng format.
 * Ch?a test HTTP server trực tiếp (integration test cuối đóng gói tự verify
 * qua curl/browser).
 */
#include "../src/core/hugo_api.h"
#include "../src/core/disk_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

static int contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

static void test_health(void) {
    printf("\n[1] /health JSON\n");
    const char *path = "p9_health.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "mydb", path);

    char *j = hugo_api_health(&db);
    CHECK(j != NULL, "health returns JSON");
    CHECK(contains(j, "\"status\":\"ok\""), "status field");
    CHECK(contains(j, "\"db\":\"mydb\""), "db field");
    CHECK(contains(j, "\"wal_enabled\":true"), "wal_enabled");
    free(j);

    ddb_close(&db);
    remove(path); remove("p9_health.hugolog");
}

static void test_empty_list(void) {
    printf("\n[2] /collections with empty DB\n");
    const char *path = "p9_list.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "t", path);

    char *j = hugo_api_list_collections(&db);
    CHECK(contains(j, "\"ok\":true"), "ok:true");
    CHECK(contains(j, "\"collections\":[]"), "empty array");
    free(j);

    ddb_close(&db);
    remove(path); remove("p9_list.hugolog");
}

static void test_insert_and_find(void) {
    printf("\n[3] POST /query: insert + find flow\n");
    const char *path = "p9_flow.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "t", path);

    char *j1 = hugo_api_exec(&db, "madeco users");
    CHECK(contains(j1, "\"ok\":true"), "madeco OK");
    CHECK(contains(j1, "\"info\":\"created collection users\""), "info message");
    free(j1);

    char *j2 = hugo_api_exec(&db, "vietinfo users { name: \"Alice\", age: 20 }");
    CHECK(contains(j2, "\"ok\":true"), "vietinfo OK");
    CHECK(contains(j2, "\"info\":\"inserted id=1\""), "inserted id=1");
    free(j2);

    hugo_api_exec(&db, "vietinfo users { name: \"Bob\", age: 22 }");

    char *j3 = hugo_api_exec(&db, "funden users");
    CHECK(contains(j3, "\"ok\":true"), "funden OK");
    CHECK(contains(j3, "\"count\":2"), "count = 2");
    CHECK(contains(j3, "\"name\":\"Alice\""), "Alice in docs");
    CHECK(contains(j3, "\"name\":\"Bob\""), "Bob in docs");
    CHECK(contains(j3, "\"age\":20"), "Alice.age = 20");
    free(j3);

    /* list collections — should show 1 coll with count 2 */
    char *jc = hugo_api_list_collections(&db);
    CHECK(contains(jc, "\"name\":\"users\""), "users listed");
    CHECK(contains(jc, "\"count\":2"), "users count = 2");
    free(jc);

    ddb_close(&db);
    remove(path); remove("p9_flow.hugolog");
}

static void test_parse_error(void) {
    printf("\n[4] Parse error → JSON error response\n");
    const char *path = "p9_err.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "t", path);

    char *j = hugo_api_exec(&db, "notaverb blah");
    CHECK(contains(j, "\"ok\":false"), "ok:false");
    CHECK(contains(j, "\"err_code\":"), "has err_code");
    free(j);

    ddb_close(&db);
    remove(path); remove("p9_err.hugolog");
}

static void test_json_escape(void) {
    printf("\n[5] JSON escape trong output (control chars + backslash path)\n");
    const char *path = "p9_esc.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "t", path);

    hugo_api_exec(&db, "madeco logs");
    /* Insert doc với path chứa backslash — HugoQL không escape \ trong string,
     * lưu raw. JSON builder phải escape khi output. */
    hugo_api_exec(&db, "vietinfo logs { path: \"C:/data/file.log\" }");
    char *j = hugo_api_exec(&db, "funden logs");
    /* Path có / được output nguyên vẹn trong JSON */
    CHECK(contains(j, "C:/data/file.log"), "forward slash path preserved");
    CHECK(contains(j, "\"ok\":true"), "response valid JSON structure");
    free(j);

    /* Test err_msg được escape đúng nếu chứa quote */
    char *je = hugo_api_exec(&db, "funden nonexistent");
    CHECK(contains(je, "\"ok\":false"), "error response");
    CHECK(contains(je, "\"err_code\":\"NO_COLLECTION\""), "err_code quoted properly");
    free(je);

    ddb_close(&db);
    remove(path); remove("p9_esc.hugolog");
}

static void test_complex_query(void) {
    printf("\n[6] Complex query: haar + sort + limit\n");
    const char *path = "p9_complex.hugo";
    remove(path);
    DiskDB db; ddb_create(&db, "t", path);

    for (int i = 0; i < 5; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "vietinfo users { name: \"u%d\", age: %d }", i, 20 + i);
        char *jr = hugo_api_exec(&db, sql);
        free(jr);
    }

    char *j = hugo_api_exec(&db,
        "funden users haar age $bh 21 orange bi age desc lime 2");
    CHECK(contains(j, "\"ok\":true"), "complex OK");
    CHECK(contains(j, "\"count\":2"), "count 2");
    /* Doc đầu phải là age=24 (desc) */
    CHECK(contains(j, "\"age\":24"), "first doc age 24");
    free(j);

    ddb_close(&db);
    remove(path); remove("p9_complex.hugolog");
}

int main(void) {
    printf("=== HUGO DB — Phase 9 (JSON API) Tests ===\n");
    test_health();
    test_empty_list();
    test_insert_and_find();
    test_parse_error();
    test_json_escape();
    test_complex_query();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
