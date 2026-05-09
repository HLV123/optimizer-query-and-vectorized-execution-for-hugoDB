/* test_mvcc_phase6.c — MVCC Vacuum Tests (Phase 6)
 *
 * Compile:
 *   gcc -Wall -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I src/core -I src/query \
 *       tests/test_mvcc_phase6.c \
 *       src/core/checksum.c src/core/hugo_io_posix.c src/core/page.c \
 *       src/core/wal.c src/core/disk_db.c src/core/collection.c \
 *       src/core/executor_disk.c src/core/ts_oracle.c src/core/mvcc_tx.c \
 *       src/core/doc_version.c src/core/mvcc_read.c src/core/mvcc_write.c \
 *       src/core/mvcc_recovery.c src/core/mvcc_vacuum.c \
 *       src/query/tokenizer.c src/query/parser.c src/query/executor.c \
 *       -o test_mvcc_phase6
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk_db.h"
#include "mvcc_tx.h"
#include "mvcc_read.h"
#include "mvcc_write.h"
#include "mvcc_vacuum.h"
#include "collection.h"
#include "../query/ast.h"

static int g_passed = 0, g_failed = 0;
#define TEST_BEGIN(n) printf("  [ RUN ] %s\n", (n))
#define TEST_PASS(n)  do { g_passed++; printf("  [  OK ] %s\n", (n)); } while(0)
#define TEST_FAIL(n, m) do { g_failed++; printf("  [FAIL ] %s: %s\n", (n), (m)); return; } while(0)
#define ASSERT(c, n, m) do { if (!(c)) { TEST_FAIL(n, m); } } while(0)

#define DB_PATH "test_p6.hugo"
static void cleanup(void) { remove(DB_PATH); remove("test_p6.hugolog"); }

static Document* make_doc(const char *key, const char *val) {
    Document *d = (Document*)calloc(1, sizeof(Document));
    KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
    strncpy(kv->key, key, sizeof(kv->key)-1);
    kv->value.type = VAL_STR;
    strncpy(kv->value.str, val, sizeof(kv->value.str)-1);
    d->pairs = kv; d->count = 1;
    return d;
}

static MvccTx* begin_tx(DiskDB *db) {
    MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                 ts_oracle_next(&db->mvcc_oracle), NULL, 0);
    mvcc_registry_add(&db->mvcc_registry, tx);
    return tx;
}

static void commit_tx(DiskDB *db, MvccTx *tx) {
    mvcc_commit_tx(db, tx);
    mvcc_tx_free(tx);
}

/* ===== TEST 1: vacuum removes old versions ===== */
static void test_vacuum_removes_old_versions(void) {
    const char *n = "vacuum_removes_old_versions";
    TEST_BEGIN(n);
    cleanup();

    DiskDB db;
    ddb_create(&db, "p6", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "v");

    /* Insert doc */
    MvccTx *t0 = begin_tx(&db);
    Document *d0 = make_doc("x", "v0");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "v", d0, &doc_id);
    doc_free(d0);
    commit_tx(&db, t0);

    /* Create 5 versions via updates */
    char val[32];
    for (int i = 1; i <= 5; i++) {
        MvccTx *tx = begin_tx(&db);
        snprintf(val, sizeof(val), "v%d", i);
        Document *d = make_doc("x", val);
        mvcc_update_doc(&db, tx, "v", doc_id, d);
        doc_free(d);
        commit_tx(&db, tx);
    }

    /* No active tx → oldest_visible_ts = current */
    ASSERT(db.mvcc_registry.count == 0, n, "no active tx before vacuum");

    VacuumStats stats;
    int rc = mvcc_vacuum(&db, &stats);
    ASSERT(rc == 0, n, "vacuum OK");
    ASSERT(stats.versions_removed > 0, n, "some versions removed");

    /* Latest version still readable */
    MvccTx *reader = begin_tx(&db);
    int err;
    Document *got = mvcc_find_doc(&db, reader, "v", doc_id, &err);
    ASSERT(got != NULL, n, "doc still readable after vacuum");
    if (got) {
        Value v;
        doc_get_field(got, "x", &v);
        ASSERT(strcmp(v.str, "v5") == 0, n, "latest version = v5");
        doc_free(got);
    }
    commit_tx(&db, reader);

    ddb_close(&db);
    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 2: vacuum preserves versions needed by active tx ===== */
static void test_vacuum_preserves_active_snapshot(void) {
    const char *n = "vacuum_preserves_active_snapshot";
    TEST_BEGIN(n);
    cleanup();

    DiskDB db;
    ddb_create(&db, "p6", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "items");

    /* Insert doc value="old" */
    MvccTx *t0 = begin_tx(&db);
    Document *d0 = make_doc("v", "old");
    uint64_t doc_id;
    mvcc_insert_doc(&db, t0, "items", d0, &doc_id);
    doc_free(d0);
    commit_tx(&db, t0);

    /* Long-running reader — begins BEFORE update */
    MvccTx *long_reader = begin_tx(&db);

    /* Another tx updates the doc */
    MvccTx *t2 = begin_tx(&db);
    Document *d2 = make_doc("v", "new");
    mvcc_update_doc(&db, t2, "items", doc_id, d2);
    doc_free(d2);
    commit_tx(&db, t2);

    /* Vacuum: oldest_visible_ts = long_reader.begin_ts
     * → should NOT remove "old" version (long_reader needs it) */
    VacuumStats stats;
    mvcc_vacuum(&db, &stats);

    /* Long reader must still see "old" */
    int err;
    Document *got = mvcc_find_doc(&db, long_reader, "items", doc_id, &err);
    ASSERT(got != NULL, n, "long_reader still sees doc after vacuum");
    if (got) {
        Value v;
        doc_get_field(got, "v", &v);
        ASSERT(strcmp(v.str, "old") == 0, n, "long_reader sees 'old' (snapshot preserved)");
        doc_free(got);
    }

    commit_tx(&db, long_reader);

    /* Now vacuum again — long_reader committed, no more active tx */
    VacuumStats stats2;
    mvcc_vacuum(&db, &stats2);
    /* This time old version may be removed */

    ddb_close(&db);
    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 3: oldest_visible_ts calculation ===== */
static void test_oldest_visible_ts(void) {
    const char *n = "oldest_visible_ts";
    TEST_BEGIN(n);
    cleanup();

    DiskDB db;
    ddb_create(&db, "p6", DB_PATH);
    db.mode = HUGO_MODE_MVCC;

    /* No active tx → oldest = current */
    uint64_t cur = ts_oracle_current(&db.mvcc_oracle);
    uint64_t oldest = mvcc_oldest_visible_ts(&db);
    ASSERT(oldest >= cur, n, "oldest >= current when no active tx");

    /* Add active tx with low begin_ts */
    MvccTx *tx = mvcc_tx_create(1, 5, NULL, 0);  /* begin_ts = 5 (low) */
    mvcc_registry_add(&db.mvcc_registry, tx);

    /* Force oracle to high value */
    ts_oracle_advance(&db.mvcc_oracle, 1000);

    oldest = mvcc_oldest_visible_ts(&db);
    ASSERT(oldest == 5, n, "oldest = min active begin_ts = 5");

    mvcc_registry_remove(&db.mvcc_registry, tx->tx_id);
    mvcc_tx_free(tx);

    ddb_close(&db);
    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 4: vacuum stats ===== */
static void test_vacuum_stats(void) {
    const char *n = "vacuum_stats";
    TEST_BEGIN(n);
    cleanup();

    DiskDB db;
    ddb_create(&db, "p6", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "s");

    /* Insert + 10 updates */
    MvccTx *t0 = begin_tx(&db);
    Document *d0 = make_doc("n", "0");
    uint64_t id;
    mvcc_insert_doc(&db, t0, "s", d0, &id);
    doc_free(d0);
    commit_tx(&db, t0);

    for (int i = 1; i <= 10; i++) {
        MvccTx *tx = begin_tx(&db);
        char buf[16]; snprintf(buf, sizeof(buf), "%d", i);
        Document *d = make_doc("n", buf);
        mvcc_update_doc(&db, tx, "s", id, d);
        doc_free(d);
        commit_tx(&db, tx);
    }

    ASSERT(db.mvcc_registry.count == 0, n, "no active tx");

    VacuumStats stats;
    mvcc_vacuum(&db, &stats);

    ASSERT(stats.versions_removed > 0,     n, "versions_removed > 0");
    ASSERT(stats.pages_freed > 0,          n, "pages_freed > 0");
    ASSERT(stats.oldest_visible_ts > 0,    n, "oldest_visible_ts > 0");

    printf("    (removed=%llu freed=%llu oldest_ts=%llu)\n",
           (unsigned long long)stats.versions_removed,
           (unsigned long long)stats.pages_freed,
           (unsigned long long)stats.oldest_visible_ts);

    ddb_close(&db);
    cleanup();
    TEST_PASS(n);
}

int main(void) {
    printf("\n=== Hugo DB Stage 3: Phase 6 (Vacuum) Tests ===\n\n");
    test_oldest_visible_ts();
    test_vacuum_removes_old_versions();
    test_vacuum_preserves_active_snapshot();
    test_vacuum_stats();
    printf("\n=== Results: %d/%d passed ===\n", g_passed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
