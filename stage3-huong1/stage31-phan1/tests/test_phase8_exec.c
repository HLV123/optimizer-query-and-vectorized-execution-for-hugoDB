/* test_phase8_exec.c — E2E executor_disk tests
 *
 * Giống test_phase7 nhưng dùng DiskDB (persistence thật sự qua PageManager
 * + CRC32 check mỗi page + page_type thật).
 */
#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/core/executor_disk.h"
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

static int run_query(DiskDB *db, const char *sql, HugoResult *r) {
    TokenList tl;
    if (hugo_tokenize(sql, &tl) != 0) return -1;
    Query q;
    if (hugo_parse(&tl, &q) != 0) { query_free(&q); return -1; }
    hugo_execute_disk(db, &q, r);
    query_free(&q);
    return 0;
}

/* ===== Test 1: basic E2E ===== */
static void test_basic(void) {
    printf("\n[1] Basic insert + find on DiskDB\n");
    const char *path = "p8e_basic.hugo";
    remove(path);

    DiskDB db;
    CHECK(ddb_create(&db, "test", path) == 0, "create DB");

    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    CHECK(r.ok, "insert Alice");
    result_free_disk(&r);

    run_query(&db, "vietinfo users { name: \"Bob\", age: 22 }", &r);
    CHECK(r.ok, "insert Bob");
    result_free_disk(&r);

    run_query(&db, "funden users", &r);
    CHECK(r.ok && r.count == 2, "funden → 2 docs");
    result_free_disk(&r);

    run_query(&db, "funden users haar age $bh 21", &r);
    CHECK(r.ok && r.count == 1, "haar age > 21 → 1 doc");
    Value v;
    CHECK(doc_get_field(r.docs[0], "name", &v) == 0 && strcmp(v.str, "Bob") == 0,
          "that's Bob");
    result_free_disk(&r);

    ddb_close(&db);
    remove(path);
}

/* ===== Test 2: CROSS-SESSION persistence ===== */
static void test_cross_session(void) {
    printf("\n[2] Cross-session persistence: close + reopen\n");
    const char *path = "p8e_cross.hugo";
    remove(path);

    /* Session 1 */
    {
        DiskDB db;
        ddb_create(&db, "test", path);
        HugoResult r;
        run_query(&db, "vietinfo users { name: \"Alice\", age: 20, city: \"HN\" }", &r);
        result_free_disk(&r);
        run_query(&db, "vietinfo users { name: \"Bob\", age: 22 }", &r);
        result_free_disk(&r);
        run_query(&db, "vietinfo orders { user_id: 1, total: 500 }", &r);
        result_free_disk(&r);
        run_query(&db, "cochin users haar name $bg \"Alice\" $quy age 21", &r);
        CHECK(r.ok, "cochin in session 1");
        result_free_disk(&r);
        ddb_close(&db);
    }

    /* Session 2: open file, data phải còn */
    {
        DiskDB db;
        CHECK(ddb_open(&db, path) == 0, "reopen DB file");
        CHECK(db.n_colls == 2, "2 collections reopened");

        HugoResult r;
        run_query(&db, "funden users", &r);
        CHECK(r.ok && r.count == 2, "users → 2 docs");
        result_free_disk(&r);

        run_query(&db, "funden users haar name $bg \"Alice\"", &r);
        CHECK(r.ok && r.count == 1, "Alice found");
        Value v;
        CHECK(doc_get_field(r.docs[0], "age", &v) == 0 && v.num == 21,
              "Alice.age = 21 (cochin persisted)");
        result_free_disk(&r);

        run_query(&db, "funden orders", &r);
        CHECK(r.count == 1, "orders has 1 doc");
        result_free_disk(&r);

        ddb_close(&db);
    }
    remove(path);
}

/* ===== Test 3: delete + persistence ===== */
static void test_delete_persist(void) {
    printf("\n[3] Delete + cross-session persistence\n");
    const char *path = "p8e_del.hugo";
    remove(path);

    {
        DiskDB db;
        ddb_create(&db, "test", path);
        HugoResult r;
        for (int i = 0; i < 10; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "vietinfo users { name: \"u%d\", age: %d }", i, 10 + i);
            run_query(&db, sql, &r);
            result_free_disk(&r);
        }
        run_query(&db, "demlet users haar age $lh 15", &r);
        CHECK(r.ok && r.count == 5, "deleted 5 docs (age < 15)");
        result_free_disk(&r);
        ddb_close(&db);
    }
    {
        DiskDB db;
        ddb_open(&db, path);
        HugoResult r;
        run_query(&db, "funden users", &r);
        CHECK(r.count == 5, "5 docs remain after reopen");
        result_free_disk(&r);
        ddb_close(&db);
    }
    remove(path);
}

/* ===== Test 4: sort + limit + skip on disk ===== */
static void test_sort_limit(void) {
    printf("\n[4] Sort + limit + skopan\n");
    const char *path = "p8e_sort.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    HugoResult r;
    const char *inserts[] = {
        "vietinfo users { name: \"Alice\", age: 20 }",
        "vietinfo users { name: \"Bob\",   age: 22 }",
        "vietinfo users { name: \"Carol\", age: 25 }",
        "vietinfo users { name: \"Dave\",  age: 30 }",
    };
    for (int i = 0; i < 4; i++) { run_query(&db, inserts[i], &r); result_free_disk(&r); }

    run_query(&db, "funden users orange bi age desc lime 2", &r);
    CHECK(r.ok && r.count == 2, "desc + lime 2 → 2 docs");
    Value v;
    CHECK(doc_get_field(r.docs[0], "name", &v) == 0 && strcmp(v.str, "Dave") == 0,
          "first = Dave (age 30)");
    result_free_disk(&r);

    run_query(&db, "funden users orange bi age asc lime 2 skopan 1", &r);
    CHECK(r.ok && r.count == 2, "asc + skopan 1 + lime 2 → 2 docs");
    CHECK(doc_get_field(r.docs[0], "name", &v) == 0 && strcmp(v.str, "Bob") == 0,
          "after skip 1: first = Bob");
    result_free_disk(&r);

    ddb_close(&db);
    remove(path);
}

/* ===== Test 5: CRC verify — disk tampering phát hiện được =====
 * Modify 1 byte trong data page → khi đọc lại phải fail CRC.
 */
static void test_crc_check(void) {
    printf("\n[5] CRC check — data page tampering phát hiện\n");
    const char *path = "p8e_crc.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    HugoResult r;
    run_query(&db, "vietinfo users { name: \"Secret\", code: 12345 }", &r);
    result_free_disk(&r);
    ddb_close(&db);

    /* Tamper: page 2 (doc page) offset 30 */
    FILE *f = fopen(path, "r+b");
    fseek(f, 2 * HUGO_PAGE_SIZE + HUGO_PAGE_HDR_SIZE + 10, SEEK_SET);
    uint8_t byte;
    fread(&byte, 1, 1, f);
    byte ^= 0x01;
    fseek(f, -1, SEEK_CUR);
    fwrite(&byte, 1, 1, f);
    fclose(f);

    /* Reopen: header + meta OK, nhưng đọc doc page sẽ fail CRC */
    DiskDB db2;
    ddb_open(&db2, path);
    run_query(&db2, "funden users", &r);
    /* Document không đọc được → r.count = 0 (scan skips failed read) */
    CHECK(r.count == 0, "tampered doc not readable (CRC fail skips it)");
    result_free_disk(&r);
    ddb_close(&db2);
    remove(path);
}

int main(void) {
    printf("=== HUGO DB — Phase 8.a (Integrated Disk Executor) Tests ===\n");
    test_basic();
    test_cross_session();
    test_delete_persist();
    test_sort_limit();
    test_crc_check();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
