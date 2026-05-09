/* test_phase8.c — Disk-backed database tests
 *
 * Phase 8.a acceptance:
 *   [x] Create DB → persist empty header + meta
 *   [x] Insert doc → fit 1 page → persist
 *   [x] Read doc back bit-for-bit
 *   [x] Update doc in-place
 *   [x] Delete doc
 *   [x] Close + reopen → all data còn (persistence qua PageManager)
 *   [x] Reject doc quá lớn
 */
#include "../src/core/disk_db.h"
#include "../src/core/collection.h"  /* for doc_set_field/doc_get_field/doc_free */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* Build simple Document: {name: str, age: num} */
static Document* build_doc(const char *name, double age) {
    Document *d = (Document*)calloc(1, sizeof(Document));
    Value v;
    v.type = VAL_STR; strncpy(v.str, name, sizeof(v.str)-1); v.num = 0;
    doc_set_field(d, "name", v);
    memset(&v, 0, sizeof(v));
    v.type = VAL_NUM; v.num = age;
    doc_set_field(d, "age", v);
    return d;
}

/* ===== Test 1: create + single insert ===== */
static void test_basic_insert(void) {
    printf("\n[1] Create + single insert + read back\n");
    const char *path = "phase8_basic.hugo";
    remove(path);

    DiskDB db;
    CHECK(ddb_create(&db, "test", path) == 0, "create DB");

    DiskColl *c = ddb_create_coll(&db, "users");
    CHECK(c != NULL, "create collection users");

    Document *doc = build_doc("Alice", 20);
    uint64_t id;
    CHECK(ddb_insert_doc(&db, c, doc, &id) == 0, "insert doc");
    CHECK(id == 1, "first id = 1");
    doc_free(doc);

    Document *back = ddb_read_doc(&db, c, id);
    CHECK(back != NULL, "read back");
    Value v;
    CHECK(doc_get_field(back, "name", &v) == 0 && strcmp(v.str, "Alice") == 0,
          "name = Alice");
    CHECK(doc_get_field(back, "age", &v) == 0 && v.num == 20, "age = 20");
    CHECK(doc_get_field(back, "id", &v) == 0 && v.num == 1, "auto-id = 1");
    doc_free(back);

    ddb_close(&db);
    remove(path);
}

/* ===== Test 2: persistence - close + reopen ===== */
static void test_persistence(void) {
    printf("\n[2] Persistence qua close + reopen\n");
    const char *path = "phase8_persist.hugo";
    remove(path);

    /* Session 1: insert 10 docs */
    {
        DiskDB db;
        ddb_create(&db, "test", path);
        DiskColl *c = ddb_create_coll(&db, "users");
        for (int i = 0; i < 10; i++) {
            char name[32];
            snprintf(name, sizeof(name), "user_%d", i);
            Document *d = build_doc(name, 20 + i);
            ddb_insert_doc(&db, c, d, NULL);
            doc_free(d);
        }
        CHECK(c->count == 10, "count = 10 trước close");
        ddb_close(&db);
    }

    /* Session 2: reopen và verify */
    {
        DiskDB db;
        CHECK(ddb_open(&db, path) == 0, "reopen");
        CHECK(db.n_colls == 1, "1 collection");
        DiskColl *c = ddb_get_coll(&db, "users");
        CHECK(c != NULL, "users collection exists");
        CHECK(c->count == 10, "count = 10 sau reopen");

        int matches = 0;
        for (uint64_t id = 1; id <= 10; id++) {
            Document *d = ddb_read_doc(&db, c, id);
            if (!d) continue;
            Value v;
            char expected[32];
            snprintf(expected, sizeof(expected), "user_%d", (int)(id - 1));
            if (doc_get_field(d, "name", &v) == 0 && strcmp(v.str, expected) == 0
                && doc_get_field(d, "age", &v) == 0 && v.num == (20 + (int)(id - 1)))
                matches++;
            doc_free(d);
        }
        CHECK(matches == 10, "10 docs match after reopen");

        ddb_close(&db);
    }
    remove(path);
}

/* ===== Test 3: update ===== */
static void test_update(void) {
    printf("\n[3] Update doc in-place\n");
    const char *path = "phase8_update.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    DiskColl *c = ddb_create_coll(&db, "users");

    Document *d = build_doc("Alice", 20);
    uint64_t id;
    ddb_insert_doc(&db, c, d, &id);
    doc_free(d);

    /* Update */
    Document *d2 = build_doc("Alice", 21);
    Value v; v.type = VAL_STR; strncpy(v.str, "Hanoi", sizeof(v.str)-1); v.num = 0;
    doc_set_field(d2, "city", v);
    CHECK(ddb_update_doc(&db, c, id, d2) == 0, "update OK");
    doc_free(d2);

    /* Read back */
    Document *back = ddb_read_doc(&db, c, id);
    CHECK(doc_get_field(back, "age", &v) == 0 && v.num == 21, "age updated to 21");
    CHECK(doc_get_field(back, "city", &v) == 0 && strcmp(v.str, "Hanoi") == 0,
          "city added");
    doc_free(back);

    ddb_close(&db);
    remove(path);
}

/* ===== Test 4: delete ===== */
static void test_delete(void) {
    printf("\n[4] Delete doc\n");
    const char *path = "phase8_delete.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    DiskColl *c = ddb_create_coll(&db, "users");

    uint64_t ids[5];
    for (int i = 0; i < 5; i++) {
        Document *d = build_doc("u", i);
        ddb_insert_doc(&db, c, d, &ids[i]);
        doc_free(d);
    }
    CHECK(c->count == 5, "count = 5");

    CHECK(ddb_delete_doc(&db, c, ids[2]) == 0, "delete id=3");
    CHECK(c->count == 4, "count = 4 after delete");
    CHECK(ddb_read_doc(&db, c, ids[2]) == NULL, "deleted doc returns NULL");

    /* Other docs still readable */
    Document *d = ddb_read_doc(&db, c, ids[0]);
    CHECK(d != NULL, "id=1 still readable");
    doc_free(d);

    ddb_close(&db);

    /* Persistence of deletion */
    DiskDB db2;
    ddb_open(&db2, path);
    DiskColl *c2 = ddb_get_coll(&db2, "users");
    CHECK(c2->count == 4, "count = 4 after reopen");
    CHECK(ddb_read_doc(&db2, c2, ids[2]) == NULL, "deletion persists");
    ddb_close(&db2);
    remove(path);
}

/* ===== Test 5: too large doc ===== */
static void test_too_large(void) {
    printf("\n[5] Reject doc quá lớn\n");
    const char *path = "phase8_large.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    DiskColl *c = ddb_create_coll(&db, "users");

    /* Build doc với 1 field string rất dài — overflow page */
    Document *d = (Document*)calloc(1, sizeof(Document));
    Value v; v.type = VAL_STR;
    /* sizeof(v.str) = 256, không đủ 1 page. Tạo nhiều field. */
    for (int i = 0; i < 30; i++) {
        char key[32];
        snprintf(key, sizeof(key), "field_%d", i);
        memset(v.str, 'x', sizeof(v.str)-1);
        v.str[sizeof(v.str)-1] = 0;
        doc_set_field(d, key, v);
    }
    int rc = ddb_insert_doc(&db, c, d, NULL);
    CHECK(rc == -2, "insert doc quá lớn → error");
    doc_free(d);

    ddb_close(&db);
    remove(path);
}

/* ===== Test 6: multi-collection ===== */
static void test_multi_coll(void) {
    printf("\n[6] Nhiều collection\n");
    const char *path = "phase8_multi.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    DiskColl *u = ddb_create_coll(&db, "users");
    DiskColl *o = ddb_create_coll(&db, "orders");
    DiskColl *p = ddb_create_coll(&db, "products");
    CHECK(u && o && p, "3 collections created");
    CHECK(db.n_colls == 3, "n_colls = 3");

    /* Duplicate creation → NULL */
    CHECK(ddb_create_coll(&db, "users") == NULL, "duplicate → NULL");

    Document *d1 = build_doc("Alice", 20);
    ddb_insert_doc(&db, u, d1, NULL);
    doc_free(d1);

    Document *d2 = build_doc("order_1", 500);
    ddb_insert_doc(&db, o, d2, NULL);
    doc_free(d2);

    CHECK(u->count == 1 && o->count == 1 && p->count == 0,
          "counts: 1/1/0");

    ddb_close(&db);

    /* Reopen */
    DiskDB db2;
    ddb_open(&db2, path);
    CHECK(db2.n_colls == 3, "3 colls reopened");
    CHECK(ddb_get_coll(&db2, "users")->count == 1, "users.count = 1");
    CHECK(ddb_get_coll(&db2, "orders")->count == 1, "orders.count = 1");

    /* Drop */
    CHECK(ddb_drop_coll(&db2, "products") == 0, "drop products");
    CHECK(db2.n_colls == 2, "n_colls = 2");

    ddb_close(&db2);
    remove(path);
}

/* ===== Test 7: scan ===== */
typedef struct { int count; double sum_age; } ScanCtx;
static void scan_cb(uint64_t id, Document *d, void *ctx) {
    (void)id;
    ScanCtx *sc = (ScanCtx*)ctx;
    sc->count++;
    Value v;
    if (doc_get_field(d, "age", &v) == 0) sc->sum_age += v.num;
}

static void test_scan(void) {
    printf("\n[7] Scan all docs in collection\n");
    const char *path = "phase8_scan.hugo";
    remove(path);

    DiskDB db;
    ddb_create(&db, "test", path);
    DiskColl *c = ddb_create_coll(&db, "users");
    for (int i = 0; i < 20; i++) {
        Document *d = build_doc("u", (double)i);
        ddb_insert_doc(&db, c, d, NULL);
        doc_free(d);
    }
    /* Delete một vài */
    ddb_delete_doc(&db, c, 5);
    ddb_delete_doc(&db, c, 10);

    ScanCtx sc = {0};
    ddb_scan(&db, c, scan_cb, &sc);
    CHECK(sc.count == 18, "scan visit 18 docs (20 - 2 deleted)");
    /* Sum ages: 0+1+...+19 = 190, minus 4 (i=5) minus 9 (i=10) = 177 */
    CHECK(sc.sum_age == 177, "sum of ages = 177");

    ddb_close(&db);
    remove(path);
}

int main(void) {
    printf("=== HUGO DB — Phase 8.a (Disk-Backed DB) Tests ===\n");
    test_basic_insert();
    test_persistence();
    test_update();
    test_delete();
    test_too_large();
    test_multi_coll();
    test_scan();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
