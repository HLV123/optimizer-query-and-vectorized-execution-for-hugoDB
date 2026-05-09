/* tokenizer.h — HugoQL tokenizer
 *
 * Input: "funden users haar age $bh 18 orange bi age desc lime 10"
 * Output: Token[] = [VERB_FUNDEN, IDENT("users"), KW_HAAR, IDENT("age"),
 *                    OP_BH, NUM(18), KW_ORANGE_BI, IDENT("age"), KW_DESC,
 *                    KW_LIME, NUM(10)]
 *
 * Bigram handling: "orange" + "bi" → KW_ORANGE_BI; "gremb" + "bi" → KW_GREMB_BI
 * Case-insensitive keywords (parser accept cả hoa lẫn thường).
 */
#ifndef HUGO_TOKENIZER_H
#define HUGO_TOKENIZER_H

#include <stdint.h>

typedef enum {
    /* Verbs */
    TOK_FUNDEN, TOK_VIETINFO, TOK_COCHIN, TOK_DEMLET,
    TOK_MADECO, TOK_DELCO, TOK_MADECOIDU, TOK_DELECOIDU,
    TOK_GOMAIL, TOK_GINAN, TOK_COMETI, TOK_TULABERK,
    TOK_USF, TOK_SKILL, TOK_EXEPANUS, TOK_ANALYZE,

    /* Clauses */
    TOK_HAAR,           /* WHERE */
    TOK_ORANGE_BI,      /* ORDER BY */
    TOK_LIME,           /* LIMIT */
    TOK_SKOPAN,         /* SKIP */
    TOK_GREMB_BI,       /* GROUP BY */

    /* Sort directions */
    TOK_ASC, TOK_DESC,

    /* Comparison operators */
    TOK_OP_LH, TOK_OP_BH, TOK_OP_LHB, TOK_OP_BHB,
    TOK_OP_BG, TOK_OP_KC,

    /* List/existence operators */
    TOK_OP_TG, TOK_OP_KTG, TOK_OP_TNTT, TOK_OP_XAU,

    /* Logic operators */
    TOK_OP_VAND, TOK_OP_VOR, TOK_OP_VNOT,

    /* Value operators */
    TOK_OP_QUY,         /* $set */
    TOK_OP_DON,         /* $push */
    TOK_OP_LOI,         /* $pull */
    TOK_OP_RASOAT,      /* $lookup */

    /* Aggregation */
    TOK_POU, TOK_SEP, TOK_AWR, TOK_MIE, TOK_MAF,

    /* Literals */
    TOK_STRING,         /* "..." */
    TOK_NUMBER,         /* 123, 3.14 */
    TOK_IDENT,          /* field name, collection name */

    /* Punctuation */
    TOK_LBRACE, TOK_RBRACE,     /* { } */
    TOK_LBRACKET, TOK_RBRACKET, /* [ ] */
    TOK_COMMA,                   /* , */
    TOK_COLON,                   /* : */
    TOK_DOT,                     /* . (cho dotted field: address.city) */

    /* Special */
    TOK_TU,              /* "tu" keyword in $rasoat (join syntax) */
    TOK_ON,              /* "on" keyword in $rasoat */

    TOK_EOF,
    TOK_ERROR
} HugoTokenType;

typedef struct {
    HugoTokenType type;
    char      text[256];      /* raw text (cho IDENT, STRING, NUMBER) */
    double    num_val;        /* pre-parsed nếu TOK_NUMBER */
} Token;

#define MAX_TOKENS 512

typedef struct {
    Token tokens[MAX_TOKENS];
    int   count;
    int   error;              /* 1 nếu tokenization failed */
    char  error_msg[256];
} TokenList;

/* Tokenize input string. Trả về 0 nếu OK, -1 nếu lỗi (chi tiết trong tl->error_msg). */
int hugo_tokenize(const char *input, TokenList *tl);

/* Debug: in token list */
void token_list_print(const TokenList *tl);

/* Token type → tên string (cho debug) */
const char* token_type_name(HugoTokenType t);

#endif
