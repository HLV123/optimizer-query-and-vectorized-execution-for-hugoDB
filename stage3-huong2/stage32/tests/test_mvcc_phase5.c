/* test_mvcc_phase5.c — MVCC WAL Recovery Tests (Phase 5)
 *
 * Compile:
 *   gcc -Wall -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I src/core -I src/query \
 *       tests/test_mvcc_phase5.c \
 *       src/core/checksum.c src/core/hugo_io_posix.c src/core/page.c \
 *       src/core/wal.c src/core/disk_db.c src/core/collection.c \
 *       src/core/executor_disk.c src/core/ts_oracle.c src/core/mvcc_tx.c \
 *       src/core/doc_version.c src/core/mvcc_read.c src/core/mvcc_write.c \
 *       src/core/mvcc_recovery.c src/core/mvcc_vacuum.c \
 *       src/query/tokenizer.c src/query/parser.c src/query/executor.c \
 *       -o test_mvcc_phase5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "disk_db.h"
#include "mvcc_tx.h"
#include "mvcc_read.h"
#include "mvcc_write.h"
#include "mvcc_recovery.h"
#include "collection.h"
#include "../query/ast.h"

static int g_passed = 0, g_failed = 0;

#define TEST_BEGIN(n) do { printf("  [ RUN ] %s\n", (n)); } while(0)
#define TEST_PASS(n)  do { g_passed++; printf("  [  OK ] %s\n", (n)); } while(0)
#define TEST_FAIL(n, msg) do { g_failed++; printf("  [FAIL ] %s: %s\n", (n), (msg)); return; } while(0)
#define ASSERT(cond, n, msg) do { if (!(cond)) { TEST_FAIL(n, msg); } } while(0)

#define DB_PATH "test_p5.hugo"
static void cleanup(void) { remove(DB_PATH); remove("test_p5.hugolog"); }

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
    uint64_t ids[256]; size_t n = 0;
    mvcc_registry_snapshot(&db->mvcc_registry, ids, &n, 256);
    MvccTx *tx = mvcc_tx_create(wal_new_tx_id(&db->wal),
                                 ts_oracle_next(&db->mvcc_oracle), ids, n);
    mvcc_registry_add(&db->mvcc_registry, tx);
    wal_log_mvcc_begin(&db->wal, tx->tx_id, tx->begin_ts);
    wal_sync(&db->wal);
    return tx;
}

/* ===== TEST 1: WAL MVCC records written correctly ===== */
static void test_wal_mvcc_records(void) {
    const char *n = "wal_mvcc_records";
    TEST_BEGIN(n);
    cleanup();

    DiskDB db;
    ddb_create(&db, "p5", DB_PATH);
    db.mode = HUGO_MODE_MVCC;
    ddb_create_coll(&db, "t");

    MvccTx *tx = begin_tx(&db);
    Document *d = make_doc("v", "hello");
    uint64_t id;
    mvcc_insert_doc(&db, tx, "t", d, &id);
    doc_free(d);
    mvcc_commit_tx(&db, tx);
    mvcc_tx_free(tx);
    ddb_close(&db);

    /* Scan WAL và verify records */
    DiskDB db2;
    memset(&db2, 0, sizeof(db2));
    if (wal_open(&db2.wal, "test_p5.hugolog") != WAL_OK) {
        TEST_FAIL(n, "wal_open failed");
    }

    int found_begin = 0, found_commit = 0, found_version = 0;
    WalIter it; wal_iter_init(&it, &db2.wal);
    WalRecord rec;
    while (wal_iter_next(&it, &rec) == WAL_OK) {
        if (rec.type == WAL_MVCC_BEGIN)   found_begin   = 1;
        if (rec.type == WAL_MVCC_COMMIT)  found_commit  = 1;
        if (rec.type == WAL_MVCC_VERSION) found_version = 1;
    }
    wal_close(&db2.wal);

    ASSERT(found_begin,   n, "WAL_MVCC_BEGIN found");
    ASSERT(found_commit,  n, "WAL_MVCC_COMMIT found");
    ASSERT(found_version, n, "WAL_MVCC_VERSION found");

    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 2: Recovery restores committed data ===== */
static void test_recovery_committed(void) {
    const char *n = "recovery_committed";
    TEST_BEGIN(n);
    cleanup();

    /* Phase 1: insert + commit */
    {
        DiskDB db;
        ddb_create(&db, "p5", DB_PATH);
        db.mode = HUGO_MODE_MVCC;
        ddb_create_coll(&db, "docs");

        MvccTx *tx = begin_tx(&db);
        Document *d = make_doc("content", "persistent");
        uint64_t id;
        mvcc_insert_doc(&db, tx, "docs", d, &id);
        doc_free(d);
        mvcc_commit_tx(&db, tx);
        mvcc_tx_free(tx);
        /* Close WITHOUT clearing WAL (simulate no checkpoint) */
        /* ddb_close calls save_meta but doesn't truncate WAL */
        ddb_close(&db);
    }

    /* Phase 2: reopen → recovery runs */
    {
        DiskDB db;
        ddb_open_mode(&db, DB_PATH, HUGO_MODE_MVCC);

        /* Verify doc 1 visible */
        MvccTx *tx = begin_tx(&db);
        int err;
        Document *got = mvcc_find_doc(&db, tx, "docs", 1, &err);
        ASSERT(got != NULL, n, "doc visible after recovery");
        if (got) {
            Value v;
            int rc = doc_get_field(got, "content", &v);
            ASSERT(rc == 0 && strcmp(v.str, "persistent") == 0,
                   n, "content='persistent' after recovery");
            doc_free(got);
        }
        mvcc_commit_tx(&db, tx);
        mvcc_tx_free(tx);
        ddb_close(&db);
    }

    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 3: Recovery rolls back uncommitted (loser) tx ===== */
static void test_recovery_loser_tx(void) {
    const char *n = "recovery_loser_tx";
    TEST_BEGIN(n);
    cleanup();

    uint64_t seed_id = 0;

    /* Phase 1: seed doc */
    {
        DiskDB db;
        ddb_create(&db, "p5", DB_PATH);
        db.mode = HUGO_MODE_MVCC;
        ddb_create_coll(&db, "items");
        MvccTx *t0 = begin_tx(&db);
        Document *d = make_doc("val", "original");
        mvcc_insert_doc(&db, t0, "items", d, &seed_id);
        doc_free(d);
        mvcc_commit_tx(&db, t0);
        mvcc_tx_free(t0);
        ddb_close(&db);
    }

    /* Phase 2: simulate crash mid-tx (BEGIN + write but no COMMIT) */
    {
        DiskDB db;
        ddb_open_mode(&db, DB_PATH, HUGO_MODE_MVCC);
        MvccTx *tx = begin_tx(&db);
        Document *d = make_doc("val", "crashed_write");
        mvcc_update_doc(&db, tx, "items", seed_id, d);
        doc_free(d);
        /* DO NOT commit — simulate crash by just closing without committing */
        /* We need to flush the WAL log for the version */
        /* The WAL_MVCC_VERSION already written by mvcc_update_doc */
        /* But no WAL_MVCC_COMMIT → loser tx */
        /* Don't call mvcc_commit_tx or mvcc_abort_tx */
        /* Just close file handles to simulate crash */
        wal_close(&db.wal);
        pm_close(&db.pm);
        /* Free tx without going through clean path */
        mvcc_tx_free(tx);
    }

    /* Phase 3: reopen → recovery should rollback the loser tx */
    {
        DiskDB db;
        ddb_open_mode(&db, DB_PATH, HUGO_MODE_MVCC);

        MvccTx *tx = begin_tx(&db);
        int err;
        Document *got = mvcc_find_doc(&db, tx, "items", seed_id, &err);
        ASSERT(got != NULL, n, "doc still exists after recovery");
        if (got) {
            Value v;
            int rc = doc_get_field(got, "val", &v);
            ASSERT(rc == 0 && strcmp(v.str, "original") == 0,
                   n, "loser tx rolled back, sees original");
            doc_free(got);
        }
        mvcc_commit_tx(&db, tx);
        mvcc_tx_free(tx);
        ddb_close(&db);
    }

    cleanup();
    TEST_PASS(n);
}

/* ===== TEST 4: TsOracle advances after recovery ===== */
static void test_ts_oracle_advances_after_recovery(void) {
    const char *n = "ts_oracle_advances";
    TEST_BEGIN(n);
    cleanup();

    uint64_t ts_before_close = 0;

    {
        DiskDB db;
        ddb_create(&db, "p5", DB_PATH);
        db.mode = HUGO_MODE_MVCC;
        ddb_create_coll(&db, "x");
        MvccTx *tx = begin_tx(&db);
        Document *d = make_doc("k", "v");
        uint64_t id;
        mvcc_insert_doc(&db, tx, "x", d, &id);
        doc_free(d);
        mvcc_commit_tx(&db, tx);
        ts_before_close = tx->commit_ts;
        mvcc_tx_free(tx);
        ddb_close(&db);
    }

    {
        DiskDB db;
        ddb_open_mode(&db, DB_PATH, HUGO_MODE_MVCC);
        uint64_t ts_after = ts_oracle_next(&db.mvcc_oracle);
        ASSERT(ts_after > ts_before_close, n,
               "oracle advanced past pre-close ts after recovery");
        ddb_close(&db);
    }

    cleanup();
    TEST_PASS(n);
}

int main(void) {
    printf("\n=== Hugo DB Stage 3: Phase 5 (WAL Recovery) Tests ===\n\n");
    test_wal_mvcc_records();
    test_recovery_committed();
    test_recovery_loser_tx();
    test_ts_oracle_advances_after_recovery();
    printf("\n=== Results: %d/%d passed ===\n", g_passed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
