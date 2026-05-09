/* tokenizer.c — HugoQL tokenizer */
#include "tokenizer.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* Keyword table — case insensitive */
typedef struct { const char *word; HugoTokenType type; } KWEntry;

static const KWEntry keywords[] = {
    /* Verbs */
    {"funden",    TOK_FUNDEN},    {"vietinfo",  TOK_VIETINFO},
    {"cochin",    TOK_COCHIN},    {"demlet",    TOK_DEMLET},
    {"madeco",    TOK_MADECO},    {"delco",     TOK_DELCO},
    {"madecoidu", TOK_MADECOIDU},{"delecoidu", TOK_DELECOIDU},
    {"gomail",    TOK_GOMAIL},    {"ginan",     TOK_GINAN},
    {"cometi",    TOK_COMETI},    {"tulaberk",  TOK_TULABERK},
    {"usf",       TOK_USF},      {"skill",     TOK_SKILL},
    {"exepanus",  TOK_EXEPANUS},
    /* Clauses */
    {"haar",      TOK_HAAR},     {"lime",      TOK_LIME},
    {"skopan",    TOK_SKOPAN},
    /* Sort */
    {"asc",       TOK_ASC},      {"desc",      TOK_DESC},
    /* Aggregation */
    {"pou",       TOK_POU},      {"sep",       TOK_SEP},
    {"awr",       TOK_AWR},      {"mie",       TOK_MIE},
    {"maf",       TOK_MAF},
    /* Join helpers */
    {"tu",        TOK_TU},       {"on",        TOK_ON},
    {NULL, TOK_EOF}
};

/* Operator table — dùng cho $xxx */
static const KWEntry operators[] = {
    {"$lh",      TOK_OP_LH},     {"$bh",      TOK_OP_BH},
    {"$lhb",     TOK_OP_LHB},    {"$bhb",     TOK_OP_BHB},
    {"$bg",      TOK_OP_BG},     {"$kc",      TOK_OP_KC},
    {"$tg",      TOK_OP_TG},     {"$ktg",     TOK_OP_KTG},
    {"$tntt",    TOK_OP_TNTT},   {"$xau",     TOK_OP_XAU},
    {"$vand",    TOK_OP_VAND},   {"$vor",     TOK_OP_VOR},
    {"$vnot",    TOK_OP_VNOT},
    {"$quy",     TOK_OP_QUY},    {"$don",     TOK_OP_DON},
    {"$loi",     TOK_OP_LOI},    {"$rasoat",  TOK_OP_RASOAT},
    {NULL, TOK_EOF}
};

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static HugoTokenType lookup_keyword(const char *w) {
    for (int i = 0; keywords[i].word; i++)
        if (streq_ci(w, keywords[i].word)) return keywords[i].type;
    return TOK_IDENT;
}

static HugoTokenType lookup_operator(const char *w) {
    for (int i = 0; operators[i].word; i++)
        if (streq_ci(w, operators[i].word)) return operators[i].type;
    return TOK_ERROR;
}

static void add_token(TokenList *tl, HugoTokenType type, const char *text) {
    if (tl->count >= MAX_TOKENS) { tl->error = 1; return; }
    Token *t = &tl->tokens[tl->count++];
    t->type = type;
    t->num_val = 0;
    if (text) {
        size_t len = strlen(text);
        if (len >= sizeof(t->text)) len = sizeof(t->text) - 1;
        memcpy(t->text, text, len);
        t->text[len] = 0;
    } else {
        t->text[0] = 0;
    }
}

static void add_num_token(TokenList *tl, const char *text, double val) {
    if (tl->count >= MAX_TOKENS) { tl->error = 1; return; }
    Token *t = &tl->tokens[tl->count++];
    t->type = TOK_NUMBER;
    t->num_val = val;
    size_t len = strlen(text);
    if (len >= sizeof(t->text)) len = sizeof(t->text) - 1;
    memcpy(t->text, text, len);
    t->text[len] = 0;
}

int hugo_tokenize(const char *input, TokenList *tl) {
    memset(tl, 0, sizeof(*tl));
    const char *p = input;

    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* String literal */
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            size_t len = (size_t)(p - start);
            char buf[256];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len);
            buf[len] = 0;
            add_token(tl, TOK_STRING, buf);
            if (*p == '"') p++;
            continue;
        }

        /* Punctuation */
        if (*p == '{') { add_token(tl, TOK_LBRACE, "{"); p++; continue; }
        if (*p == '}') { add_token(tl, TOK_RBRACE, "}"); p++; continue; }
        if (*p == '[') { add_token(tl, TOK_LBRACKET, "["); p++; continue; }
        if (*p == ']') { add_token(tl, TOK_RBRACKET, "]"); p++; continue; }
        if (*p == ',') { add_token(tl, TOK_COMMA, ","); p++; continue; }
        if (*p == ':') { add_token(tl, TOK_COLON, ":"); p++; continue; }

        /* Number (including negative) */
        if (isdigit((unsigned char)*p) ||
            (*p == '-' && isdigit((unsigned char)*(p+1)))) {
            const char *start = p;
            if (*p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.' && isdigit((unsigned char)*(p+1))) {
                p++;
                while (isdigit((unsigned char)*p)) p++;
            }
            size_t len = (size_t)(p - start);
            char buf[64];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len);
            buf[len] = 0;
            add_num_token(tl, buf, atof(buf));
            continue;
        }

        /* $ operator */
        if (*p == '$') {
            const char *start = p;
            p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t len = (size_t)(p - start);
            char buf[64];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len);
            buf[len] = 0;
            HugoTokenType tt = lookup_operator(buf);
            if (tt == TOK_ERROR) {
                snprintf(tl->error_msg, sizeof(tl->error_msg),
                         "unknown operator: %s", buf);
                tl->error = 1;
                return -1;
            }
            add_token(tl, tt, buf);
            continue;
        }

        /* Word (keyword/ident) — bao gồm dotted fields (address.city) */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) p++;
            size_t len = (size_t)(p - start);
            char buf[256];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len);
            buf[len] = 0;
            /* Lookup keyword */
            HugoTokenType tt = lookup_keyword(buf);
            add_token(tl, tt, buf);
            continue;
        }

        /* Unknown char */
        snprintf(tl->error_msg, sizeof(tl->error_msg),
                 "unexpected char: '%c' (0x%02X)", *p, (unsigned char)*p);
        tl->error = 1;
        return -1;
    }

    /* Bigram merge pass: "orange" + "bi" → KW_ORANGE_BI, "gremb" + "bi" → KW_GREMB_BI */
    for (int i = 0; i < tl->count - 1; i++) {
        if (tl->tokens[i].type == TOK_IDENT && tl->tokens[i+1].type == TOK_IDENT) {
            if (streq_ci(tl->tokens[i].text, "orange") &&
                streq_ci(tl->tokens[i+1].text, "bi")) {
                tl->tokens[i].type = TOK_ORANGE_BI;
                snprintf(tl->tokens[i].text, sizeof(tl->tokens[i].text), "orange bi");
                /* Remove token i+1 */
                for (int j = i + 1; j < tl->count - 1; j++)
                    tl->tokens[j] = tl->tokens[j + 1];
                tl->count--;
            }
            else if (streq_ci(tl->tokens[i].text, "gremb") &&
                     streq_ci(tl->tokens[i+1].text, "bi")) {
                tl->tokens[i].type = TOK_GREMB_BI;
                snprintf(tl->tokens[i].text, sizeof(tl->tokens[i].text), "gremb bi");
                for (int j = i + 1; j < tl->count - 1; j++)
                    tl->tokens[j] = tl->tokens[j + 1];
                tl->count--;
            }
        }
    }

    add_token(tl, TOK_EOF, "");
    return 0;
}

const char* token_type_name(HugoTokenType t) {
    switch (t) {
    case TOK_FUNDEN: return "FUNDEN"; case TOK_VIETINFO: return "VIETINFO";
    case TOK_COCHIN: return "COCHIN"; case TOK_DEMLET: return "DEMLET";
    case TOK_MADECO: return "MADECO"; case TOK_DELCO: return "DELCO";
    case TOK_MADECOIDU: return "MADECOIDU"; case TOK_DELECOIDU: return "DELECOIDU";
    case TOK_GOMAIL: return "GOMAIL"; case TOK_GINAN: return "GINAN";
    case TOK_COMETI: return "COMETI"; case TOK_TULABERK: return "TULABERK";
    case TOK_USF: return "USF"; case TOK_SKILL: return "SKILL";
    case TOK_EXEPANUS: return "EXEPANUS";
    case TOK_HAAR: return "HAAR"; case TOK_ORANGE_BI: return "ORANGE_BI";
    case TOK_LIME: return "LIME"; case TOK_SKOPAN: return "SKOPAN";
    case TOK_GREMB_BI: return "GREMB_BI";
    case TOK_ASC: return "ASC"; case TOK_DESC: return "DESC";
    case TOK_OP_LH: return "$lh"; case TOK_OP_BH: return "$bh";
    case TOK_OP_LHB: return "$lhb"; case TOK_OP_BHB: return "$bhb";
    case TOK_OP_BG: return "$bg"; case TOK_OP_KC: return "$kc";
    case TOK_OP_TG: return "$tg"; case TOK_OP_KTG: return "$ktg";
    case TOK_OP_TNTT: return "$tntt"; case TOK_OP_XAU: return "$xau";
    case TOK_OP_VAND: return "$vand"; case TOK_OP_VOR: return "$vor";
    case TOK_OP_VNOT: return "$vnot";
    case TOK_OP_QUY: return "$quy"; case TOK_OP_DON: return "$don";
    case TOK_OP_LOI: return "$loi"; case TOK_OP_RASOAT: return "$rasoat";
    case TOK_POU: return "POU"; case TOK_SEP: return "SEP";
    case TOK_AWR: return "AWR"; case TOK_MIE: return "MIE"; case TOK_MAF: return "MAF";
    case TOK_STRING: return "STRING"; case TOK_NUMBER: return "NUMBER";
    case TOK_IDENT: return "IDENT";
    case TOK_LBRACE: return "{"; case TOK_RBRACE: return "}";
    case TOK_LBRACKET: return "["; case TOK_RBRACKET: return "]";
    case TOK_COMMA: return ","; case TOK_COLON: return ":";
    case TOK_DOT: return ".";
    case TOK_TU: return "TU"; case TOK_ON: return "ON";
    case TOK_EOF: return "EOF"; case TOK_ERROR: return "ERROR";
    }
    return "???";
}

void token_list_print(const TokenList *tl) {
    for (int i = 0; i < tl->count; i++) {
        const Token *t = &tl->tokens[i];
        if (t->type == TOK_NUMBER)
            printf("[%s %s=%.6g] ", token_type_name(t->type), t->text, t->num_val);
        else if (t->type == TOK_STRING || t->type == TOK_IDENT)
            printf("[%s \"%s\"] ", token_type_name(t->type), t->text);
        else
            printf("[%s] ", token_type_name(t->type));
    }
    printf("\n");
}
