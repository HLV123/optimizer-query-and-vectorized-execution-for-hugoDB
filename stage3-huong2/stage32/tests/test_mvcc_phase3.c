/* test_mvcc_phase3.c — Tests cho MVCC read path (Stage 3, Phase 3)
 *
 * Compile:
 *   gcc -Wall -O2 -std=c11 -I src/core -I src/query \
 *       tests/test_mvcc_phase3.c \
 *       src/core/checksum.c src/core/hugo_io_posix.c src/core/page.c \
 *       src/core/wal.c src/core/disk_db.c src/core/collection.c \
 *       src/core/executor_disk.c src/core/ts_oracle.c src/core/mvcc_tx.c \
 *       src/core/doc_version.c src/core/mvcc_read.c src/core/mvcc_write.c \
 *       src/query/tokenizer.c src/query/parser.c src/query/executor.c \
 *       -o test_mvcc_phase3
 *
 * Mỗi test dọn dẹp file db tạm sau khi chạy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "disk_db.h"
#include "mvcc_tx.h"
#include "mvcc_read.h"
#include "mvcc_write.h"
#include "ts_oracle.h"
#include "collection.h"
#include "../query/ast.h"

/* ===== Test infrastructure ===== */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name) \
    do { \
        g_tests_run++; \
        printf("  [ RUN ] %s\n", (name)); \
    } while(0)

#define TEST_PASS(name) \
    do { \
        g_tests_passed++; \
        printf("  [  OK ] %s\n", (name)); \
    } while(0)

#define TEST_FAIL(name, msg) \
    do { \
        g_tests_failed++; \
        printf("  [FAIL ] %s: %s\n", (name), (msg)); \
    } while(0)

#define ASSERT_TRUE(cond, name, msg) \
    do { if (!(cond)) { TEST_FAIL(name, msg); return; } } while(0)

#define DB_PATH  "test_mvcc_p3.hugo"

static void cleanup_db(void) {
    remove(DB_PATH);
    remove(DB_PATH ".hugolog");
    /* Derive log path: test_mvcc_p3.hugolog */
    remove("test_mvcc_p3.hugolog");
}

/* Helper: tạo Document đơn giản với 1 string field */
static Document* make_doc(const char *key, const char *val) {
    Document *d = (Document*)calloc(1, sizeof(Document));
    KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
    strncpy(kv->key, key, sizeof(kv->key)-1);
    kv->value.type = VAL_STR;
    strncpy(kv->value.str, val, sizeof(kv->value.str)-1);
    d->pairs = kv;
    d->count = 1;
    return d;
}

/* Helper: tạo MvccTx đơn giản với begin_ts cho sẵn */
static MvccTx* make_tx(DiskDB *db, uint64_t begin_ts,
                        const uint64_t *active_set, size_t n_active) {
    uint64_t tx_id = wal_new_tx_id(&db->wal);
    MvccTx *tx = mvcc_tx_create(tx_id, begin_ts, active_set, n_active);
    if (tx) mvcc_registry_add(&db->mvcc_registry, tx);
    return tx;
}

static void finish_tx(DiskDB *db, MvccTx *tx, int commit) {
    if (commit) mvcc_commit_tx(db, tx);
    else        mvcc_abort_tx(db, tx);
    mvcc_tx_free(tx);
}

/* Helper: lấy string field từ doc */
static int doc_str_field(const Document *doc, const char *key, char *out, size_t out_size) {
    Value v;
    if (doc_get_field(doc, key, &v) != 0) return -1;
    if (v.type != VAL_STR) return -1;
    strncpy(out, v.str, out_size-1);
    out[out_size-1] = '\0';
    return 0;
}

/* ================================================================
 * TEST 1: TsOracle — monotonic timestamps
 * ================================================================ */
static void test_ts_oracle_monotonic(void) {
    const char *name = "ts_oracle_monotonic";
    TEST_BEGIN(name);

    TsOracle oracle;
    ts_oracle_init(&oracle, 1);

    uint64_t t1 = ts_oracle_next(&oracle);
    uint64_t t2 = ts_oracle_next(&oracle);
    uint64_t t3 = ts_oracle_next(&oracle);

    ASSERT_TRUE(t1 < t2, name, "t1 < t2");
    ASSERT_TRUE(t2 < t3, name, "t2 < t3");
    ASSERT_TRUE(t1 >= 1, name,  "t1 >= 1 (start from TS_MIN)");

    TEST_PASS(name);
}

/* ================================================================
 * TEST 2: DocVersion serialize/deserialize round-trip
 * ================================================================ */
static void test_doc_version_roundtrip(void) {
    const char *name = "doc_version_roundtrip";
    TEST_BEGIN(name);

    uint8_t raw_data[] = { 0x00, 0x02,  /* 2 fields */
        0x00, 0x03, 'n', 'a', 'm', 0x02, 0x00, 0x05, 'a', 'l', 'i', 'c', 'e',
        0x00, 0x03, 'a', 'g', 'e', 0x01, 0x40, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    DocVersion v;
    memset(&v, 0, sizeof(v));
    v.version_id       = 42;
    v.created_ts       = 100;
    v.deleted_ts       = 0;
    v.created_tx       = 7;
    v.prev_version_ptr = 0;
    v.data_size        = 3;

    uint8_t small_data[3] = { 0xAA, 0xBB, 0xCC };
    uint8_t buf[256];

    int written = doc_version_serialize(&v, small_data, buf, sizeof(buf));
    ASSERT_TRUE(written == (int)(DOC_VERSION_HDR_SIZE + 3), name, "serialize size");

    DocVersion v2;
    uint8_t data2[16];
    int read = doc_version_deserialize(buf, (size_t)written, &v2, data2, sizeof(data2));
    ASSERT_TRUE(read == written, name, "deserialize returns same size");
    ASSERT_TRUE(v2.version_id       == 42,  name, "version_id matches");
    ASSERT_TRUE(v2.created_ts       == 100, name, "created_ts matches");
    ASSERT_TRUE(v2.deleted_ts       == 0,   name, "deleted_ts matches");
    ASSERT_TRUE(v2.created_tx       == 7,   name, "created_tx matches");
    ASSERT_TRUE(v2.prev_version_ptr == 0,   name, "prev_ptr matches");
    ASSERT_TRUE(v2.data_size        == 3,   name, "data_size matches");
    ASSERT_TRUE(memcmp(data2, small_data, 3) == 0, name, "data bytes match");

    TEST_PASS(name);
    (void)raw_data;
}

/* ================================================================
 * TEST 3: mvcc_tx_create + active_set snapshot
 * ================================================================ */
static void test_mvcc_tx_active_set(void) {
    const char *name = "mvcc_tx_active_set";
    TEST_BEGIN(name);

    uint64_t active[3] = { 10, 20, 30 };
    MvccTx *tx = mvcc_tx_create(99, 50, active, 3);
    ASSERT_TRUE(tx != NULL, name, "tx created");
    ASSERT_TRUE(tx->tx_id    == 99, name, "tx_id");
    ASSERT_TRUE(tx->begin_ts == 50, name, "begin_ts");
    ASSERT_TRUE(tx->n_active == 3,  name, "n_active");
    ASSERT_TRUE(tx->active_set[0] == 10 &&
                tx->active_set[1] == 20 &&
                tx->active_set[2] == 30, name, "active_set values");
    ASSERT_TRUE(tx->state == MVCC_TX_ACTIVE, name, "state=ACTIVE");

    mvcc_tx_free(tx);
    TEST_PASS(name);
}

/* ================================================================
 * TEST 4: Registry add/remove/find
 * ================================================================ */
static void test_mvcc_registry(void) {
    const char *name = "mvcc_registry";
    TEST_BEGIN(name);

    MvccTxRegistry reg;
    mvcc_registry_init(&reg);

    MvccTx *t1 = mvcc_tx_create(1, 10, NULL, 0);
    MvccTx *t2 = mvcc_tx_create(2, 20, NULL, 0);
    MvccTx *t3 = mvcc_tx_create(3, 30, NULL, 0);

    mvcc_registry_add(&reg, t1);
    mvcc_registry_add(&reg, t2);
    mvcc_registry_add(&reg, t3);

    ASSERT_TRUE(reg.count == 3, name, "count=3");
    ASSERT_TRUE(mvcc_registry_find(&reg, 2) == t2, name, "find t2");
    ASSERT_TRUE(mvcc_registry_is_active(&reg, 3) == 1, name, "t3 active");

    mvcc_registry_remove(&reg, 2);
    ASSERT_TRUE(reg.count == 2, name, "count=2 after remove");
    ASSERT_TRUE(mvcc_registry_find(&reg, 2) == NULL, name, "t2 not found after remove");
    ASSERT_TRUE(mvcc_registry_is_active(&reg, 1) == 1, name, "t1 still active");

    /* Snapshot test */
    uint64_t ids[4];
    size_t n = 0;
    mvcc_registry_snapshot(&reg, ids, &n, 4);
    ASSERT_TRUE(n == 2, name, "snapshot n=2");

    mvcc_tx_free(t1);
    mvcc_tx_free(t2);
    mvcc_tx_free(t3);
    TEST_PASS(name);
}

/* ================================================================
 * TEST 5: version_ptr encoding/decoding
 * ================================================================ */
static void test_version_ptr_encoding(void) {
    const char *name = "version_ptr_encoding";
    TEST_BEGIN(name);

    uint64_t page_id = 12345;
    uint16_t offset  = 0;
    uint64_t ptr = version_ptr_encode(page_id, offset);

    ASSERT_TRUE(version_ptr_page(ptr)   == page_id, name, "page_id round-trip");
    ASSERT_TRUE(version_ptr_offset(ptr) == offset,  name, "offset round-trip");
    ASSERT_TRUE(ptr != VERSION_PTR_NULL, name, "not NULL");

    /* Test offset > 0 */
    uint64_t ptr2 = version_ptr_encode(7, 128);
    ASSERT_TRUE(version_ptr_page(ptr2)   == 7,   name, "page_id=7");
    ASSERT_TRUE(version_ptr_offset(ptr2) == 128, name, "offset=128");

    TEST_PASS(name);
}

/* ================================================================
 * TEST 6: MVCC DB open in MVCC mode
 * ================================================================ */
static void test_mvcc_db_open(void) {
    const char *name = "mvcc_db_open";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    int rc = ddb_create(&db, "test_mvcc_p3", DB_PATH);
    ASSERT_TRUE(rc == 0, name, "ddb_create OK");

    db.mode = HUGO_MODE_MVCC;
    ASSERT_TRUE(db.mode == HUGO_MODE_MVCC, name, "mode=MVCC");

    /* Oracle bắt đầu từ 1 */
    uint64_t t1 = ts_oracle_next(&db.mvcc_oracle);
    uint64_t t2 = ts_oracle_next(&db.mvcc_oracle);
    ASSERT_TRUE(t1 < t2, name, "oracle monotonic on fresh db");

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 7: INSERT + read own writes (chưa commit)
 * ================================================================ */
static void test_read_own_writes(void) {
    const char *name = "read_own_writes";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "users");

    /* Begin tx */
    MvccTx *tx = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    ASSERT_TRUE(tx != NULL, name, "tx created");

    /* Insert document */
    Document *doc = make_doc("name", "Alice");
    uint64_t id;
    int rc = mvcc_insert_doc(&db, tx, "users", doc, &id);
    doc_free(doc);
    ASSERT_TRUE(rc == MVCC_OK, name, "insert OK");
    ASSERT_TRUE(id > 0, name, "id > 0");

    /* Đọc lại chính doc vừa insert (chưa commit — own write) */
    int err;
    Document *got = mvcc_find_doc(&db, tx, "users", id, &err);
    ASSERT_TRUE(got != NULL, name, "read own write returns doc");
    ASSERT_TRUE(err == 0,   name, "no error");

    char val[64];
    rc = doc_str_field(got, "name", val, sizeof(val));
    ASSERT_TRUE(rc == 0,                  name, "has 'name' field");
    ASSERT_TRUE(strcmp(val, "Alice") == 0, name, "value='Alice'");

    doc_free(got);
    finish_tx(&db, tx, 1);  /* commit */

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 8: Snapshot isolation — T2 không thấy update của T3
 * ================================================================ */
static void test_snapshot_isolation(void) {
    const char *name = "snapshot_isolation";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "users");

    /* T1: insert doc value="A" + commit */
    MvccTx *t1 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *doc_a = make_doc("name", "A");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t1, "users", doc_a, &doc_id);
    doc_free(doc_a);
    finish_tx(&db, t1, 1);  /* commit */

    /* T2: begin — snapshot sau commit của T1 */
    MvccTx *t2 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);

    /* Verify T2 thấy "A" */
    int err;
    Document *read1 = mvcc_find_doc(&db, t2, "users", doc_id, &err);
    ASSERT_TRUE(read1 != NULL, name, "T2 sees doc");
    char val[64];
    doc_str_field(read1, "name", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "A") == 0, name, "T2 sees value=A");
    doc_free(read1);

    /* T3: update doc value="B" + commit */
    MvccTx *t3 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *doc_b = make_doc("name", "B");
    int update_rc = mvcc_update_doc(&db, t3, "users", doc_id, doc_b);
    doc_free(doc_b);
    ASSERT_TRUE(update_rc == MVCC_OK, name, "T3 update OK");
    finish_tx(&db, t3, 1);  /* commit */

    /* T2 đọc lại — PHẢI vẫn thấy "A" (snapshot isolation!) */
    Document *read2 = mvcc_find_doc(&db, t2, "users", doc_id, &err);
    ASSERT_TRUE(read2 != NULL, name, "T2 still sees doc after T3 commit");
    doc_str_field(read2, "name", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "A") == 0, name, "T2 still sees A (snapshot isolation)");
    doc_free(read2);

    finish_tx(&db, t2, 1);  /* commit T2 */

    /* T4: begin sau T3 committed — phải thấy "B" */
    MvccTx *t4 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *read3 = mvcc_find_doc(&db, t4, "users", doc_id, &err);
    ASSERT_TRUE(read3 != NULL, name, "T4 sees doc");
    doc_str_field(read3, "name", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "B") == 0, name, "T4 sees B (latest committed)");
    doc_free(read3);
    finish_tx(&db, t4, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 9: Aborted transaction — data không visible
 * ================================================================ */
static void test_abort_not_visible(void) {
    const char *name = "abort_not_visible";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "users");

    /* T1: insert doc + ABORT */
    MvccTx *t1 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *doc = make_doc("name", "Ghost");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t1, "users", doc, &doc_id);
    doc_free(doc);
    finish_tx(&db, t1, 0);  /* ABORT */

    /* T2: begin sau abort — không thấy doc */
    MvccTx *t2 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    int err;
    Document *got = mvcc_find_doc(&db, t2, "users", doc_id, &err);
    ASSERT_TRUE(got == NULL, name, "aborted doc not visible");
    /* err=1 (not found) hoặc err=-1 (IO) đều acceptable khi abort undo */
    finish_tx(&db, t2, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 10: Write-write conflict detection
 * ================================================================ */
static void test_ww_conflict(void) {
    const char *name = "ww_conflict";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "items");

    /* T0: seed document */
    MvccTx *t0 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *seed = make_doc("val", "original");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "items", seed, &doc_id);
    doc_free(seed);
    finish_tx(&db, t0, 1);

    /* T1 và T2 concurrent — snapshot tại CÙNG thời điểm */
    uint64_t snap = ts_oracle_next(&db.mvcc_oracle);
    MvccTx *t1 = make_tx(&db, snap, NULL, 0);
    MvccTx *t2 = make_tx(&db, snap, NULL, 0);

    /* T1 update trước */
    Document *d1 = make_doc("val", "from_T1");
    int rc1 = mvcc_update_doc(&db, t1, "items", doc_id, d1);
    doc_free(d1);
    ASSERT_TRUE(rc1 == MVCC_OK, name, "T1 update OK");

    /* T1 commit trước */
    finish_tx(&db, t1, 1);

    /* T2 cũng update cùng doc — phải conflict vì T1 đã committed với commit_ts > snap */
    Document *d2 = make_doc("val", "from_T2");
    int rc2 = mvcc_update_doc(&db, t2, "items", doc_id, d2);
    doc_free(d2);
    ASSERT_TRUE(rc2 == MVCC_ERR_CONFLICT, name, "T2 gets conflict");
    ASSERT_TRUE(t2->state == MVCC_TX_ABORTED, name, "T2 state=ABORTED");

    finish_tx(&db, t2, 0);  /* abort T2 */

    /* T3: đọc sau cùng — phải thấy "from_T1" */
    MvccTx *t3 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    int err;
    Document *got = mvcc_find_doc(&db, t3, "items", doc_id, &err);
    ASSERT_TRUE(got != NULL, name, "T3 sees doc");
    char val[64];
    doc_str_field(got, "val", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "from_T1") == 0, name, "T3 sees from_T1 (winner)");
    doc_free(got);
    finish_tx(&db, t3, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 11: Readers không block lẫn nhau (no lock)
 * ================================================================ */
static void test_concurrent_readers_no_block(void) {
    const char *name = "concurrent_readers_no_block";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "data");

    /* Seed */
    MvccTx *t0 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *seed = make_doc("x", "hello");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "data", seed, &doc_id);
    doc_free(seed);
    finish_tx(&db, t0, 1);

    /* T1 và T2 đọc song song — cả 2 đều thấy doc */
    MvccTx *t1 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    MvccTx *t2 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);

    int err1, err2;
    Document *r1 = mvcc_find_doc(&db, t1, "data", doc_id, &err1);
    Document *r2 = mvcc_find_doc(&db, t2, "data", doc_id, &err2);

    ASSERT_TRUE(r1 != NULL && err1 == 0, name, "T1 reads OK");
    ASSERT_TRUE(r2 != NULL && err2 == 0, name, "T2 reads OK (no blocking)");

    doc_free(r1);
    doc_free(r2);
    finish_tx(&db, t1, 1);
    finish_tx(&db, t2, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 12: Writer không block readers (readers thấy old version)
 * ================================================================ */
static void test_writer_not_blocking_readers(void) {
    const char *name = "writer_not_blocking_readers";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "docs");

    /* Seed: value="old" */
    MvccTx *t0 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *seed = make_doc("v", "old");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "docs", seed, &doc_id);
    doc_free(seed);
    finish_tx(&db, t0, 1);

    /* Writer T1: update value="new" nhưng CHƯA commit */
    uint64_t w_snap = ts_oracle_next(&db.mvcc_oracle);
    MvccTx *writer = make_tx(&db, w_snap, NULL, 0);
    Document *new_doc = make_doc("v", "new");
    mvcc_update_doc(&db, writer, "docs", doc_id, new_doc);
    doc_free(new_doc);
    /* writer còn active, chưa commit */

    /* Reader T2: begin sau writer — nhưng writer chưa commit
     * → snapshot phải thấy "old" version */
    /* Lúc này writer đang active → nằm trong active_set của reader */
    uint64_t active_ids[1] = { writer->tx_id };
    MvccTx *reader = make_tx(&db, ts_oracle_next(&db.mvcc_oracle),
                              active_ids, 1);

    int err;
    Document *got = mvcc_find_doc(&db, reader, "docs", doc_id, &err);
    ASSERT_TRUE(got != NULL, name, "reader gets doc (not blocked by writer)");
    char val[64];
    doc_str_field(got, "v", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "old") == 0, name, "reader sees OLD version (writer uncommitted)");
    doc_free(got);

    finish_tx(&db, reader, 1);
    finish_tx(&db, writer, 1);  /* now writer commits */

    /* T3: begin sau writer commit — thấy "new" */
    MvccTx *t3 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *got2 = mvcc_find_doc(&db, t3, "docs", doc_id, &err);
    ASSERT_TRUE(got2 != NULL, name, "T3 sees doc");
    doc_str_field(got2, "v", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "new") == 0, name, "T3 sees NEW version");
    doc_free(got2);
    finish_tx(&db, t3, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 13: DELETE visibility
 * ================================================================ */
static void test_delete_visibility(void) {
    const char *name = "delete_visibility";
    TEST_BEGIN(name);
    cleanup_db();

    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "tbl");

    /* Insert */
    MvccTx *t0 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *seed = make_doc("item", "X");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "tbl", seed, &doc_id);
    doc_free(seed);
    finish_tx(&db, t0, 1);

    /* T1: begin TRƯỚC delete */
    MvccTx *t1 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);

    /* T2: delete + commit */
    MvccTx *t2 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    int del_rc = mvcc_delete_doc(&db, t2, "tbl", doc_id);
    ASSERT_TRUE(del_rc == MVCC_OK, name, "delete OK");
    finish_tx(&db, t2, 1);

    /* T1 đọc — PHẢI vẫn thấy doc (snapshot trước khi delete) */
    int err;
    Document *r1 = mvcc_find_doc(&db, t1, "tbl", doc_id, &err);
    ASSERT_TRUE(r1 != NULL, name, "T1 sees doc before delete committed");
    char val[64];
    doc_str_field(r1, "item", val, sizeof(val));
    ASSERT_TRUE(strcmp(val, "X") == 0, name, "T1 sees original value X");
    doc_free(r1);
    finish_tx(&db, t1, 1);

    /* T3: begin SAU delete — không thấy doc */
    MvccTx *t3 = make_tx(&db, ts_oracle_next(&db.mvcc_oracle), NULL, 0);
    Document *r3 = mvcc_find_doc(&db, t3, "tbl", doc_id, &err);
    ASSERT_TRUE(r3 == NULL, name, "T3 does not see deleted doc");
    finish_tx(&db, t3, 1);

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * TEST 14: Committed table — lookup aborted tx
 * ================================================================ */
static void test_committed_table(void) {
    const char *name = "committed_table";
    TEST_BEGIN(name);

    MvccCommittedTable tbl;
    mvcc_committed_table_init(&tbl);

    /* Thêm committed tx */
    mvcc_committed_table_add(&tbl, 100, 200, 0);
    /* Thêm aborted tx */
    mvcc_committed_table_add(&tbl, 101, 0, 1);

    const MvccTxRecord *r1 = mvcc_committed_table_find(&tbl, 100);
    ASSERT_TRUE(r1 != NULL,      name, "find tx 100");
    ASSERT_TRUE(!r1->aborted,    name, "tx 100 not aborted");
    ASSERT_TRUE(r1->commit_ts == 200, name, "tx 100 commit_ts=200");

    const MvccTxRecord *r2 = mvcc_committed_table_find(&tbl, 101);
    ASSERT_TRUE(r2 != NULL,   name, "find tx 101");
    ASSERT_TRUE(r2->aborted,  name, "tx 101 aborted");

    const MvccTxRecord *r3 = mvcc_committed_table_find(&tbl, 999);
    ASSERT_TRUE(r3 == NULL, name, "unknown tx returns NULL");

    TEST_PASS(name);
}

/* ================================================================
 * TEST 15: ddb_open_mode sets MVCC mode
 * ================================================================ */
static void test_ddb_open_mode(void) {
    const char *name = "ddb_open_mode";
    TEST_BEGIN(name);
    cleanup_db();

    /* Create fresh */
    DiskDB db;
    ddb_create(&db, "test_mvcc_p3", DB_PATH);
    ddb_close(&db);

    /* Reopen với MVCC mode */
    int rc = ddb_open_mode(&db, DB_PATH, HUGO_MODE_MVCC);
    ASSERT_TRUE(rc == 0, name, "ddb_open_mode OK");
    ASSERT_TRUE(db.mode == HUGO_MODE_MVCC, name, "mode=MVCC after open");

    ddb_close(&db);
    cleanup_db();
    TEST_PASS(name);
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(void) {
    printf("\n=== Hugo DB Stage 3: MVCC Read Path Tests ===\n\n");

    /* Unit tests (không cần DB trên disk) */
    printf("--- Unit tests ---\n");
    test_ts_oracle_monotonic();
    test_doc_version_roundtrip();
    test_mvcc_tx_active_set();
    test_mvcc_registry();
    test_version_ptr_encoding();
    test_committed_table();

    /* Integration tests (cần DB trên disk) */
    printf("\n--- Integration tests ---\n");
    test_mvcc_db_open();
    test_ddb_open_mode();
    test_read_own_writes();
    test_snapshot_isolation();
    test_abort_not_visible();
    test_ww_conflict();
    test_concurrent_readers_no_block();
    test_writer_not_blocking_readers();
    test_delete_visibility();

    /* Summary */
    printf("\n=== Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf(" ===\n");

    return g_tests_failed > 0 ? 1 : 0;
}
