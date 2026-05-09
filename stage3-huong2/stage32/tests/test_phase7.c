/* test_phase7.c — Executor integration tests
 *
 * End-to-end: string → tokenize → parse → execute → verify result.
 * Test các query thực tế từ spec section 3.3.
 */
#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/query/executor.h"
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

/* Run 1 query end-to-end */
static int run_query(HugoDatabase *db, const char *sql, HugoResult *r) {
    TokenList tl;
    if (hugo_tokenize(sql, &tl) != 0) return -1;
    Query q;
    if (hugo_parse(&tl, &q) != 0) { query_free(&q); return -1; }
    hugo_execute(db, &q, r);
    query_free(&q);
    return 0;
}

/* ===== Test 1: vietinfo + funden basic ===== */
static void test_insert_find(void) {
    printf("\n[1] vietinfo + funden\n");
    HugoDatabase db; db_init(&db, "test");

    HugoResult r;
    CHECK(run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r) == 0
          && r.ok, "insert Alice");
    CHECK(run_query(&db, "vietinfo users { name: \"Bob\", age: 22 }", &r) == 0
          && r.ok, "insert Bob");

    run_query(&db, "funden users", &r);
    CHECK(r.ok && r.count == 2, "funden users → 2 docs");

    db_free(&db);
}

/* ===== Test 2: haar filter ===== */
static void test_haar(void) {
    printf("\n[2] haar filter\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\",   age: 22 }", &r);
    run_query(&db, "vietinfo users { name: \"Carol\", age: 25 }", &r);

    run_query(&db, "funden users haar age $bh 21", &r);
    CHECK(r.ok && r.count == 2, "age > 21 → 2 docs (Bob, Carol)");

    run_query(&db, "funden users haar age $bg 20", &r);
    CHECK(r.ok && r.count == 1, "age = 20 → 1 doc (Alice)");

    run_query(&db, "funden users haar name $bg \"Bob\"", &r);
    CHECK(r.ok && r.count == 1, "name = Bob → 1 doc");

    run_query(&db, "funden users haar age $bh 21 $vand name $bg \"Carol\"", &r);
    CHECK(r.ok && r.count == 1, "compound AND → 1 doc (Carol)");

    db_free(&db);
}

/* ===== Test 3: $xau substring ===== */
static void test_xau(void) {
    printf("\n[3] $xau substring match\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\" }", &r);
    run_query(&db, "vietinfo users { name: \"Alien\" }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\" }", &r);

    run_query(&db, "funden users haar name $xau \"Ali\"", &r);
    CHECK(r.ok && r.count == 2, "$xau \"Ali\" → 2 (Alice, Alien)");

    db_free(&db);
}

/* ===== Test 4: $tntt exists ===== */
static void test_tntt(void) {
    printf("\n[4] $tntt exists\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", bio: \"hello\" }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\" }", &r);

    run_query(&db, "funden users haar bio $tntt", &r);
    CHECK(r.ok && r.count == 1, "bio $tntt → 1 doc");

    db_free(&db);
}

/* ===== Test 5: sort / limit / skip ===== */
static void test_sort_limit(void) {
    printf("\n[5] orange bi + lime + skopan\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\",   age: 22 }", &r);
    run_query(&db, "vietinfo users { name: \"Carol\", age: 25 }", &r);
    run_query(&db, "vietinfo users { name: \"Dave\",  age: 30 }", &r);

    run_query(&db, "funden users orange bi age desc lime 2", &r);
    CHECK(r.ok && r.count == 2, "desc + lime 2 → 2 docs");
    /* Kiểm tra doc đầu là Dave (age 30) */
    Value v;
    CHECK(doc_get_field(r.docs[0], "name", &v) == 0 && strcmp(v.str, "Dave") == 0,
          "first doc = Dave (highest age)");

    run_query(&db, "funden users orange bi age asc lime 2 skopan 1", &r);
    CHECK(r.ok && r.count == 2, "asc + lime 2 + skopan 1 → 2 docs");
    CHECK(doc_get_field(r.docs[0], "name", &v) == 0 && strcmp(v.str, "Bob") == 0,
          "after skip 1 asc: first = Bob");

    db_free(&db);
}

/* ===== Test 6: cochin update ===== */
static void test_cochin(void) {
    printf("\n[6] cochin update\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\",   age: 22 }", &r);

    run_query(&db, "cochin users haar age $lh 21 $quy status \"minor\"", &r);
    CHECK(r.ok && r.count == 1, "updated 1 doc");

    run_query(&db, "funden users haar name $bg \"Alice\"", &r);
    CHECK(r.ok && r.count == 1, "Alice found");
    Value v;
    CHECK(doc_get_field(r.docs[0], "status", &v) == 0 && strcmp(v.str, "minor") == 0,
          "Alice.status = minor");

    /* Bob không có status */
    run_query(&db, "funden users haar name $bg \"Bob\"", &r);
    CHECK(doc_get_field(r.docs[0], "status", &v) != 0, "Bob vẫn không có status");

    db_free(&db);
}

/* ===== Test 7: demlet delete ===== */
static void test_demlet(void) {
    printf("\n[7] demlet delete\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;
    for (int i = 0; i < 5; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "vietinfo users { name: \"u%d\", age: %d }", i, 10 + i);
        run_query(&db, sql, &r);
    }
    run_query(&db, "demlet users haar age $lh 13", &r);
    CHECK(r.ok && r.count == 3, "deleted 3 docs (age < 13)");

    run_query(&db, "funden users", &r);
    CHECK(r.count == 2, "2 docs remaining");

    db_free(&db);
}

/* ===== Test 8: madeco / delco / skill ===== */
static void test_coll_ops(void) {
    printf("\n[8] madeco / delco / skill\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    run_query(&db, "madeco users", &r);
    CHECK(r.ok, "madeco users");
    run_query(&db, "madeco orders", &r);
    CHECK(r.ok, "madeco orders");
    run_query(&db, "madeco users", &r);
    CHECK(!r.ok && strstr(r.err_code, "EXISTS"), "duplicate madeco → EXISTS");

    run_query(&db, "skill", &r);
    CHECK(r.ok && r.count == 2, "skill → 2 collections");

    run_query(&db, "delco orders", &r);
    CHECK(r.ok, "delco orders");
    run_query(&db, "skill", &r);
    CHECK(r.count == 1, "skill → 1 collection after delco");

    db_free(&db);
}

/* ===== Test 9: persistence — save + load ===== */
static void test_persistence(void) {
    printf("\n[9] Persistence: save + load\n");
    const char *path = "phase7_persist.hugo";
    remove(path);

    HugoDatabase db; db_init(&db, "mydb");
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    run_query(&db, "vietinfo users { name: \"Bob\", age: 22 }", &r);
    run_query(&db, "vietinfo orders { user_id: 1, total: 500 }", &r);

    CHECK(db_save(&db, path) == 0, "save OK");
    db_free(&db);

    /* Reload */
    HugoDatabase db2; db_init(&db2, "mydb");
    CHECK(db_load(&db2, path) == 0, "load OK");
    CHECK(db2.n_collections == 2, "2 collections reloaded");

    run_query(&db2, "funden users", &r);
    CHECK(r.count == 2, "users vẫn có 2 docs sau reload");

    Value v;
    run_query(&db2, "funden users haar name $bg \"Alice\"", &r);
    CHECK(r.count == 1 && doc_get_field(r.docs[0], "age", &v) == 0 && v.num == 20,
          "Alice.age = 20 sau reload");

    db_free(&db2);
    remove(path);
}

/* ===== Test 10: error cases ===== */
static void test_errors(void) {
    printf("\n[10] Error cases\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    run_query(&db, "funden no_such_collection", &r);
    CHECK(!r.ok && strcmp(r.err_code, "NO_COLLECTION") == 0, "NO_COLLECTION");

    run_query(&db, "vietinfo users { name: \"a\" }", &r);
    run_query(&db, "cochin users haar id $bg 99 $quy age 20", &r);
    CHECK(!r.ok && strcmp(r.err_code, "NOT_FOUND") == 0, "NOT_FOUND cochin");

    db_free(&db);
}

/* ===== Test 11: case insensitive + edge syntax ===== */
static void test_case_and_edges(void) {
    printf("\n[11] Case insensitive + edge syntax\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    run_query(&db, "VIETINFO users { name: \"X\" }", &r);
    CHECK(r.ok, "VIETINFO upper case OK (verb case-insensitive)");

    run_query(&db, "FUNDEN users", &r);
    CHECK(r.ok && r.count == 1, "FUNDEN (verb upper) + users (lower) OK");

    /* Empty haar clause */
    run_query(&db, "funden users haar age $tntt", &r);
    CHECK(r.ok, "query chạy dù không match gì");

    db_free(&db);
}

int main(void) {
    printf("=== HUGO DB — Phase 7 (Executor Integration) Tests ===\n");
    test_insert_find();
    test_haar();
    test_xau();
    test_tntt();
    test_sort_limit();
    test_cochin();
    test_demlet();
    test_coll_ops();
    test_persistence();
    test_errors();
    test_case_and_edges();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
