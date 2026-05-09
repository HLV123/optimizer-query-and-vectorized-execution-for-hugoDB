/* test_phase6.c — HugoQL tokenizer + parser tests
 *
 * Test MỌI câu query trong spec (section 3.3).
 */
#include "../src/query/tokenizer.h"
#include "../src/query/parser.h"
#include "../src/query/ast.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define CHECK(cond, msg) do {                                       \
    tests_run++;                                                    \
    if (cond) { tests_passed++; printf("  ok  : %s\n", msg); }      \
    else      { printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

/* Helper: tokenize + parse, return 0 if both succeed */
static int tp(const char *input, Query *q) {
    TokenList tl;
    if (hugo_tokenize(input, &tl) != 0) return -1;
    return hugo_parse(&tl, q);
}

/* ===== 1. Tokenizer tests ===== */
static void test_tokenizer_basic(void) {
    printf("\n[1] Tokenizer basic\n");
    TokenList tl;
    hugo_tokenize("funden users", &tl);
    CHECK(tl.tokens[0].type == TOK_FUNDEN, "funden");
    CHECK(tl.tokens[1].type == TOK_IDENT, "users = IDENT");
    CHECK(tl.tokens[2].type == TOK_EOF, "EOF");
}

static void test_tokenizer_haar(void) {
    printf("\n[2] Tokenizer haar + operators\n");
    TokenList tl;
    hugo_tokenize("funden users haar age $bh 18", &tl);
    CHECK(tl.tokens[0].type == TOK_FUNDEN, "funden");
    CHECK(tl.tokens[2].type == TOK_HAAR, "haar");
    CHECK(tl.tokens[3].type == TOK_IDENT && strcmp(tl.tokens[3].text,"age")==0, "age");
    CHECK(tl.tokens[4].type == TOK_OP_BH, "$bh");
    CHECK(tl.tokens[5].type == TOK_NUMBER && tl.tokens[5].num_val == 18, "18");
}

static void test_tokenizer_bigram(void) {
    printf("\n[3] Tokenizer bigram: orange bi, gremb bi\n");
    TokenList tl;
    hugo_tokenize("funden users orange bi age desc", &tl);
    /* After bigram merge: [FUNDEN] [IDENT users] [ORANGE_BI] [IDENT age] [DESC] [EOF] */
    int found_ob = 0;
    for (int i = 0; i < tl.count; i++)
        if (tl.tokens[i].type == TOK_ORANGE_BI) found_ob = 1;
    CHECK(found_ob, "orange bi merged into ORANGE_BI");

    hugo_tokenize("gomail orders gremb bi user_id pou total", &tl);
    int found_gb = 0;
    for (int i = 0; i < tl.count; i++)
        if (tl.tokens[i].type == TOK_GREMB_BI) found_gb = 1;
    CHECK(found_gb, "gremb bi merged into GREMB_BI");
}

static void test_tokenizer_document(void) {
    printf("\n[4] Tokenizer JSON-like document\n");
    TokenList tl;
    hugo_tokenize("vietinfo users { name: \"Alice\", age: 20, city: \"HN\" }", &tl);
    CHECK(!tl.error, "no error");
    /* Check: VIETINFO IDENT { IDENT : STRING , IDENT : NUMBER , IDENT : STRING } EOF */
    int found_brace = 0, found_string = 0;
    for (int i = 0; i < tl.count; i++) {
        if (tl.tokens[i].type == TOK_LBRACE) found_brace++;
        if (tl.tokens[i].type == TOK_STRING) found_string++;
    }
    CHECK(found_brace >= 1, "has {");
    CHECK(found_string >= 2, "has strings");
}

static void test_tokenizer_all_verbs(void) {
    printf("\n[5] Tokenizer: all 15 verbs\n");
    const char *verbs[] = {
        "funden","vietinfo","cochin","demlet","madeco","delco",
        "madecoidu","delecoidu","gomail","ginan","cometi","tulaberk",
        "usf","skill","exepanus"
    };
    HugoTokenType expected[] = {
        TOK_FUNDEN,TOK_VIETINFO,TOK_COCHIN,TOK_DEMLET,TOK_MADECO,TOK_DELCO,
        TOK_MADECOIDU,TOK_DELECOIDU,TOK_GOMAIL,TOK_GINAN,TOK_COMETI,TOK_TULABERK,
        TOK_USF,TOK_SKILL,TOK_EXEPANUS
    };
    int all_ok = 1;
    for (int i = 0; i < 15; i++) {
        TokenList tl;
        hugo_tokenize(verbs[i], &tl);
        if (tl.tokens[0].type != expected[i]) {
            printf("    %s → %s (expected %s)\n",
                   verbs[i], token_type_name(tl.tokens[0].type),
                   token_type_name(expected[i]));
            all_ok = 0;
        }
    }
    CHECK(all_ok, "all 15 verbs tokenized correctly");
}

static void test_tokenizer_case_insensitive(void) {
    printf("\n[6] Tokenizer: case insensitive\n");
    TokenList tl;
    hugo_tokenize("FUNDEN Users HAAR Age $BH 18", &tl);
    CHECK(tl.tokens[0].type == TOK_FUNDEN, "FUNDEN upper");
    CHECK(tl.tokens[2].type == TOK_HAAR, "HAAR upper");
    CHECK(tl.tokens[4].type == TOK_OP_BH, "$BH upper");
}

/* ===== 2. Parser tests: every query in spec ===== */
static void test_parse_funden_basic(void) {
    printf("\n[7] Parse: funden users\n");
    Query q; CHECK(tp("funden users", &q) == 0, "parse OK");
    CHECK(q.verb == VERB_FUNDEN, "verb = funden");
    CHECK(strcmp(q.collection, "users") == 0, "collection = users");
    CHECK(q.haar == NULL, "no haar");
    query_free(&q);
}

static void test_parse_funden_haar(void) {
    printf("\n[8] Parse: funden users haar age $bh 18\n");
    Query q; CHECK(tp("funden users haar age $bh 18", &q) == 0, "parse OK");
    CHECK(q.haar != NULL, "has haar");
    CHECK(q.haar->type == COND_CMP, "condition type CMP");
    CHECK(strcmp(q.haar->field, "age") == 0, "field = age");
    CHECK(q.haar->op == TOK_OP_BH, "op = $bh");
    CHECK(q.haar->value.type == VAL_NUM && q.haar->value.num == 18, "value = 18");
    query_free(&q);
}

static void test_parse_funden_compound(void) {
    printf("\n[9] Parse: funden users haar age $bh 18 $vand city $bg \"HN\"\n");
    Query q;
    CHECK(tp("funden users haar age $bh 18 $vand city $bg \"HN\"", &q) == 0, "parse OK");
    CHECK(q.haar != NULL && q.haar->type == COND_AND, "compound $vand");
    query_free(&q);
}

static void test_parse_funden_full(void) {
    printf("\n[10] Parse: funden + haar + orange bi + lime + skopan\n");
    Query q;
    CHECK(tp("funden users haar age $bh 18 orange bi age desc lime 10 skopan 20", &q) == 0,
          "parse OK");
    CHECK(q.haar != NULL, "has haar");
    CHECK(q.orange_bi != NULL, "has orange_bi");
    CHECK(q.orange_bi->descending == 1, "desc");
    CHECK(q.lime == 10, "lime = 10");
    CHECK(q.skopan == 20, "skopan = 20");
    query_free(&q);
}

static void test_parse_funden_xau(void) {
    printf("\n[11] Parse: funden users haar name $xau \"Ali\"\n");
    Query q;
    CHECK(tp("funden users haar name $xau \"Ali\"", &q) == 0, "parse OK");
    CHECK(q.haar->op == TOK_OP_XAU, "op = $xau");
    CHECK(strcmp(q.haar->value.str, "Ali") == 0, "value = Ali");
    query_free(&q);
}

static void test_parse_funden_tntt(void) {
    printf("\n[12] Parse: funden users haar bio $tntt\n");
    Query q;
    CHECK(tp("funden users haar bio $tntt", &q) == 0, "parse OK");
    CHECK(q.haar->type == COND_EXISTS, "condition EXISTS");
    CHECK(q.haar->op == TOK_OP_TNTT, "op = $tntt");
    query_free(&q);
}

static void test_parse_vietinfo(void) {
    printf("\n[13] Parse: vietinfo users { name: \"Alice\", age: 20 }\n");
    Query q;
    CHECK(tp("vietinfo users { name: \"Alice\", age: 20, city: \"HN\" }", &q) == 0, "parse OK");
    CHECK(q.verb == VERB_VIETINFO, "verb = vietinfo");
    CHECK(q.payload != NULL, "has payload");
    CHECK(q.payload->count == 3, "3 pairs");
    CHECK(strcmp(q.payload->pairs->key, "name") == 0, "first key = name");
    query_free(&q);
}

static void test_parse_cochin(void) {
    printf("\n[14] Parse: cochin users haar id $bg 1 $quy age 21\n");
    Query q;
    CHECK(tp("cochin users haar id $bg 1 $quy age 21", &q) == 0, "parse OK");
    CHECK(q.verb == VERB_COCHIN, "verb = cochin");
    CHECK(q.haar != NULL, "has haar");
    CHECK(q.set_ops != NULL, "has set_ops");
    CHECK(q.set_ops->op == TOK_OP_QUY, "op = $quy");
    CHECK(strcmp(q.set_ops->field, "age") == 0, "field = age");
    CHECK(q.set_ops->value.num == 21, "value = 21");
    query_free(&q);
}

static void test_parse_cochin_don(void) {
    printf("\n[15] Parse: cochin users haar id $bg 1 $don tags \"vip\"\n");
    Query q;
    CHECK(tp("cochin users haar id $bg 1 $don tags \"vip\"", &q) == 0, "parse OK");
    CHECK(q.set_ops->op == TOK_OP_DON, "op = $don");
    query_free(&q);
}

static void test_parse_demlet(void) {
    printf("\n[16] Parse: demlet users haar id $bg 1\n");
    Query q;
    CHECK(tp("demlet users haar id $bg 1", &q) == 0, "parse OK");
    CHECK(q.verb == VERB_DEMLET, "verb = demlet");
    CHECK(q.haar != NULL, "has haar");
    query_free(&q);
}

static void test_parse_collection_ops(void) {
    printf("\n[17] Parse: madeco/delco/madecoidu/delecoidu\n");
    Query q;
    CHECK(tp("madeco users", &q) == 0 && q.verb == VERB_MADECO, "madeco");
    query_free(&q);
    CHECK(tp("delco users", &q) == 0 && q.verb == VERB_DELCO, "delco");
    query_free(&q);
    CHECK(tp("madecoidu users.age", &q) == 0 && q.verb == VERB_MADECOIDU, "madecoidu");
    query_free(&q);
    CHECK(tp("delecoidu users.age", &q) == 0 && q.verb == VERB_DELECOIDU, "delecoidu");
    query_free(&q);
}

static void test_parse_gomail(void) {
    printf("\n[18] Parse: gomail orders gremb bi user_id pou total\n");
    Query q;
    CHECK(tp("gomail orders gremb bi user_id pou total", &q) == 0, "parse OK");
    CHECK(q.verb == VERB_GOMAIL, "verb = gomail");
    CHECK(q.gremb_bi != NULL, "has gremb_bi");
    CHECK(strcmp(q.gremb_bi->field, "user_id") == 0, "group field = user_id");
    CHECK(q.gremb_bi->n_aggs == 1, "1 agg func");
    CHECK(q.gremb_bi->agg_funcs[0] == TOK_POU, "agg = pou");
    query_free(&q);
}

static void test_parse_gomail_multi(void) {
    printf("\n[19] Parse: gomail orders gremb bi city sep total, awr total\n");
    Query q;
    CHECK(tp("gomail orders gremb bi city sep total, awr total", &q) == 0, "parse OK");
    CHECK(q.gremb_bi->n_aggs == 2, "2 agg funcs");
    CHECK(q.gremb_bi->agg_funcs[0] == TOK_SEP, "first = sep");
    CHECK(q.gremb_bi->agg_funcs[1] == TOK_AWR, "second = awr");
    query_free(&q);
}

static void test_parse_transaction(void) {
    printf("\n[20] Parse: ginan / cometi / tulaberk\n");
    Query q;
    CHECK(tp("ginan", &q) == 0 && q.verb == VERB_GINAN, "ginan");
    query_free(&q);
    CHECK(tp("cometi", &q) == 0 && q.verb == VERB_COMETI, "cometi");
    query_free(&q);
    CHECK(tp("tulaberk", &q) == 0 && q.verb == VERB_TULABERK, "tulaberk");
    query_free(&q);
}

static void test_parse_db_ops(void) {
    printf("\n[21] Parse: usf / skill / exepanus\n");
    Query q;
    CHECK(tp("usf mydb", &q) == 0 && q.verb == VERB_USF, "usf mydb");
    CHECK(strcmp(q.collection, "mydb") == 0, "db = mydb");
    query_free(&q);
    CHECK(tp("skill", &q) == 0 && q.verb == VERB_SKILL, "skill (no arg)");
    query_free(&q);
    CHECK(tp("skill users", &q) == 0, "skill users");
    CHECK(strcmp(q.collection, "users") == 0, "arg = users");
    query_free(&q);
    CHECK(tp("exepanus funden users haar age $bh 18", &q) == 0, "exepanus");
    query_free(&q);
}

static void test_parse_orange_bi_multi(void) {
    printf("\n[22] Parse: orange bi age desc, name asc\n");
    Query q;
    CHECK(tp("funden users haar age $bh 18 orange bi age desc, name asc", &q) == 0,
          "parse OK");
    CHECK(q.orange_bi != NULL, "has sort");
    CHECK(q.orange_bi->descending == 1, "first = desc");
    CHECK(q.orange_bi->next != NULL, "has second sort");
    CHECK(q.orange_bi->next->descending == 0, "second = asc");
    query_free(&q);
}

static void test_parse_errors(void) {
    printf("\n[23] Parse errors\n");
    Query q;
    CHECK(tp("", &q) != 0, "empty → error");
    query_free(&q);
    CHECK(tp("blahblah", &q) != 0, "unknown verb → error");
    query_free(&q);
    TokenList tl;
    CHECK(hugo_tokenize("funden users haar age $zzz 5", &tl) != 0,
          "unknown operator $zzz → tokenizer error");
}

int main(void) {
    printf("=== HUGO DB — Phase 6 (HugoQL Tokenizer + Parser) Tests ===\n");
    test_tokenizer_basic();
    test_tokenizer_haar();
    test_tokenizer_bigram();
    test_tokenizer_document();
    test_tokenizer_all_verbs();
    test_tokenizer_case_insensitive();
    test_parse_funden_basic();
    test_parse_funden_haar();
    test_parse_funden_compound();
    test_parse_funden_full();
    test_parse_funden_xau();
    test_parse_funden_tntt();
    test_parse_vietinfo();
    test_parse_cochin();
    test_parse_cochin_don();
    test_parse_demlet();
    test_parse_collection_ops();
    test_parse_gomail();
    test_parse_gomail_multi();
    test_parse_transaction();
    test_parse_db_ops();
    test_parse_orange_bi_multi();
    test_parse_errors();
    printf("\n=== Result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
