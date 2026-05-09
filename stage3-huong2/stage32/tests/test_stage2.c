/* test_stage2.c — Stage 2 tests: all 5 phases */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/query/executor.h"
#include "../src/core/collection.h"

static int tests_run = 0, tests_pass = 0;

#define TEST(name) do { printf("  %-50s", name); tests_run++; } while(0)
#define PASS() do { tests_pass++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void exec_query(HugoDatabase *db, const char *ql, HugoResult *r) {
    TokenList tl; hugo_tokenize(ql, &tl);
    Query q; hugo_parse(&tl, &q);
    hugo_execute(db, &q, r);
    query_free(&q);
}

/* ===== Phase A: $vnot, $tg, $ktg, $don, $loi ===== */
static void test_phase_a(void) {
    printf("\n=== Phase A: Missing Operators ===\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    /* Setup: insert test data */
    exec_query(&db, "vietinfo users { name: \"Alice\", age: 20, city: \"HN\" }", &r);
    exec_query(&db, "vietinfo users { name: \"Bob\", age: 25, city: \"HCM\" }", &r);
    exec_query(&db, "vietinfo users { name: \"Carol\", age: 18, city: \"HN\" }", &r);
    exec_query(&db, "vietinfo users { name: \"Dave\", age: 30, city: \"DN\" }", &r);

    /* Test $vnot */
    TEST("$vnot age $bh 20");
    exec_query(&db, "funden users haar $vnot age $bh 20", &r);
    assert(r.ok);
    assert(r.count == 2); /* Alice(20) and Carol(18) — NOT (age > 20) */
    PASS();

    /* Test $tg single value */
    TEST("$tg single value");
    exec_query(&db, "funden users haar city $tg \"HN\"", &r);
    assert(r.ok);
    assert(r.count == 2); /* Alice, Carol */
    PASS();

    /* Test $tg value list */
    TEST("$tg value list [HN, DN]");
    exec_query(&db, "funden users haar city $tg [\"HN\", \"DN\"]", &r);
    assert(r.ok);
    assert(r.count == 3); /* Alice, Carol, Dave */
    PASS();

    /* Test $ktg */
    TEST("$ktg [HN, DN]");
    exec_query(&db, "funden users haar city $ktg [\"HN\", \"DN\"]", &r);
    assert(r.ok);
    assert(r.count == 1); /* Bob only */
    PASS();

    /* Test $tg numeric list */
    TEST("$tg numeric [20, 30]");
    exec_query(&db, "funden users haar age $tg [20, 30]", &r);
    assert(r.ok);
    assert(r.count == 2); /* Alice, Dave */
    PASS();

    /* Test $don (push to array) */
    TEST("$don tags push");
    exec_query(&db, "cochin users haar name $bg \"Alice\" $don tags \"admin\"", &r);
    assert(r.ok && r.count == 1);
    exec_query(&db, "cochin users haar name $bg \"Alice\" $don tags \"vip\"", &r);
    assert(r.ok && r.count == 1);
    /* Verify */
    exec_query(&db, "funden users haar name $bg \"Alice\"", &r);
    assert(r.ok && r.count == 1);
    Value v;
    assert(doc_get_field(r.docs[0], "tags", &v) == 0);
    assert(v.type == VAL_STR);
    assert(strcmp(v.str, "admin,vip") == 0);
    PASS();

    /* Test $loi (pull from array) */
    TEST("$loi tags pull");
    exec_query(&db, "cochin users haar name $bg \"Alice\" $loi tags \"admin\"", &r);
    assert(r.ok);
    exec_query(&db, "funden users haar name $bg \"Alice\"", &r);
    assert(r.ok && r.count == 1);
    assert(doc_get_field(r.docs[0], "tags", &v) == 0);
    assert(strcmp(v.str, "vip") == 0);
    PASS();

    db_free(&db);
}

/* ===== Phase B: Batch insert + dotted path ===== */
static void test_phase_b(void) {
    printf("\n=== Phase B: Batch Insert + Dotted Path ===\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    /* Test batch insert */
    TEST("batch insert [...]");
    exec_query(&db, "vietinfo products [ { name: \"A\", price: 10 }, { name: \"B\", price: 20 }, { name: \"C\", price: 30 } ]", &r);
    assert(r.ok);
    assert(r.count == 3);
    PASS();

    TEST("verify batch count");
    exec_query(&db, "funden products", &r);
    assert(r.ok && r.count == 3);
    PASS();

    /* Test dotted path */
    TEST("dotted path $quy");
    exec_query(&db, "vietinfo users { name: \"Alice\", age: 20 }", &r);
    exec_query(&db, "cochin users haar name $bg \"Alice\" $quy address.city \"HCM\"", &r);
    assert(r.ok);
    exec_query(&db, "funden users haar address.city $bg \"HCM\"", &r);
    assert(r.ok && r.count == 1);
    PASS();

    db_free(&db);
}

/* ===== Phase C: Aggregation ===== */
static void test_phase_c(void) {
    printf("\n=== Phase C: Aggregation (gomail) ===\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    /* Setup: insert orders */
    exec_query(&db, "vietinfo orders { user: \"Alice\", city: \"HN\", total: 100 }", &r);
    exec_query(&db, "vietinfo orders { user: \"Bob\", city: \"HN\", total: 200 }", &r);
    exec_query(&db, "vietinfo orders { user: \"Carol\", city: \"HCM\", total: 150 }", &r);
    exec_query(&db, "vietinfo orders { user: \"Dave\", city: \"HCM\", total: 250 }", &r);
    exec_query(&db, "vietinfo orders { user: \"Eve\", city: \"DN\", total: 300 }", &r);

    /* Test gomail with pou (COUNT) */
    TEST("gomail pou (COUNT)");
    exec_query(&db, "gomail orders gremb bi city pou total", &r);
    assert(r.ok);
    assert(r.count == 3); /* HN, HCM, DN */
    PASS();

    /* Test gomail with sep (SUM) */
    TEST("gomail sep (SUM)");
    exec_query(&db, "gomail orders gremb bi city sep total", &r);
    assert(r.ok);
    assert(r.count == 3);
    /* Verify sum values - find HN group */
    int found_hn = 0;
    for (int i = 0; i < r.count; i++) {
        Value cv;
        if (doc_get_field(r.docs[i], "city", &cv) == 0 && cv.type == VAL_STR && strcmp(cv.str, "HN") == 0) {
            Value sv;
            if (doc_get_field(r.docs[i], "sep_total", &sv) == 0) {
                assert(sv.num == 300); /* 100 + 200 */
                found_hn = 1;
            }
        }
    }
    assert(found_hn);
    PASS();

    /* Test gomail with haar filter */
    TEST("gomail with haar filter");
    exec_query(&db, "gomail orders haar total $bh 100 gremb bi city pou total", &r);
    assert(r.ok);
    /* Filter removes Alice(100), remaining: Bob(200,HN), Carol(150,HCM), Dave(250,HCM), Eve(300,DN) */
    assert(r.count == 3); /* HN(1), HCM(2), DN(1) */
    PASS();

    /* Test gomail with multiple agg funcs */
    TEST("gomail sep + mie + maf");
    exec_query(&db, "gomail orders gremb bi city sep total, mie total, maf total", &r);
    assert(r.ok);
    assert(r.count == 3);
    PASS();

    /* Free agg result docs (owned by executor) */
    for (int i = 0; i < r.count; i++) if (r.docs[i]) doc_free(r.docs[i]);

    db_free(&db);
}

/* ===== Phase D+E: Parse tests ===== */
static void test_phase_de_parse(void) {
    printf("\n=== Phase D+E: Parse Tests ===\n");
    HugoDatabase db; db_init(&db, "test");
    HugoResult r;

    /* Test exepanus parse */
    TEST("exepanus parse");
    TokenList tl; hugo_tokenize("exepanus funden users haar age $bh 18", &tl);
    Query q; hugo_parse(&tl, &q);
    assert(q.verb == VERB_EXEPANUS);
    query_free(&q);
    PASS();

    /* Test madecoidu parse */
    TEST("madecoidu parse");
    hugo_tokenize("madecoidu users.age", &tl);
    hugo_parse(&tl, &q);
    assert(q.verb == VERB_MADECOIDU);
    assert(strcmp(q.collection, "users.age") == 0);
    query_free(&q);
    PASS();

    /* Test transaction parse */
    TEST("ginan/cometi parse");
    exec_query(&db, "ginan", &r);
    assert(r.ok);
    exec_query(&db, "cometi", &r);
    assert(r.ok);
    PASS();

    /* Test tulaberk */
    TEST("tulaberk parse");
    exec_query(&db, "ginan", &r);
    assert(r.ok);
    exec_query(&db, "tulaberk", &r);
    assert(r.ok);
    PASS();

    /* Test usf */
    TEST("usf parse");
    exec_query(&db, "usf mydb", &r);
    assert(r.ok);
    PASS();

    /* Test gomail parse */
    TEST("gomail multi-agg parse");
    hugo_tokenize("gomail orders gremb bi city sep total, awr total", &tl);
    hugo_parse(&tl, &q);
    assert(q.verb == VERB_GOMAIL);
    assert(q.gremb_bi != NULL);
    assert(q.gremb_bi->n_aggs == 2);
    query_free(&q);
    PASS();

    /* Test $rasoat parse */
    TEST("$rasoat join parse");
    hugo_tokenize("funden orders haar user_id $bg 1 $rasoat user tu users on user_id $bg users.id", &tl);
    hugo_parse(&tl, &q);
    assert(q.verb == VERB_FUNDEN);
    assert(q.join != NULL);
    assert(strcmp(q.join->alias, "user") == 0);
    assert(strcmp(q.join->target_coll, "users") == 0);
    assert(strcmp(q.join->local_field, "user_id") == 0);
    query_free(&q);
    PASS();

    db_free(&db);
}

int main(void) {
    printf("=== HUGO DB Stage 2 Tests ===\n");

    test_phase_a();
    test_phase_b();
    test_phase_c();
    test_phase_de_parse();

    printf("\n========================================\n");
    printf("  %d / %d tests passed\n", tests_pass, tests_run);
    printf("========================================\n");

    return (tests_pass == tests_run) ? 0 : 1;
}
