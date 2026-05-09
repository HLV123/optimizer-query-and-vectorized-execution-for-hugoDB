/* ast.h — HugoQL AST
 *
 * Query = { verb, collection, haar?, set_ops?, payload?, orange_bi?, lime?, skopan?, gremb_bi? }
 */
#ifndef HUGO_AST_H
#define HUGO_AST_H

#include <stdint.h>
#include "tokenizer.h"

/* ===== Value ===== */
typedef enum { VAL_NULL, VAL_NUM, VAL_STR, VAL_BOOL, VAL_DOC } ValType;

/* Forward declare for nested doc support */
struct KVPair;

typedef struct {
    ValType type;
    double  num;
    char    str[256];
    struct KVPair *doc_pairs;  /* for VAL_DOC: nested document pairs */
    int     doc_count;         /* for VAL_DOC: number of pairs */
} Value;

/* ===== Condition (haar clause) ===== */
typedef enum {
    COND_CMP,     /* field op value */
    COND_AND,     /* left $vand right */
    COND_OR,      /* left $vor right */
    COND_NOT,     /* $vnot child */
    COND_EXISTS,  /* field $tntt */
    COND_IN,      /* field $tg/$ktg [val1, val2, ...] */
} CondType;

typedef struct Condition {
    CondType         type;
    char             field[128];
    HugoTokenType        op;           /* TOK_OP_BG, TOK_OP_BH, etc. */
    Value            value;
    Value            values[64];      /* for COND_IN: value list */
    int              n_values;        /* for COND_IN: count */
    struct Condition *left;
    struct Condition *right;
} Condition;

/* ===== Set operation ($quy/$don/$loi) ===== */
typedef struct SetOp {
    HugoTokenType    op;               /* TOK_OP_QUY, TOK_OP_DON, TOK_OP_LOI */
    char         field[128];
    Value        value;
    struct SetOp *next;
} SetOp;

/* ===== Sort spec ===== */
typedef struct SortField {
    char    field[128];
    int     descending;            /* 1 = desc, 0 = asc */
    struct SortField *next;
} SortField;

/* ===== Group by ===== */
typedef struct {
    char      field[128];          /* group by field */
    HugoTokenType agg_funcs[16];       /* TOK_POU, TOK_SEP, ... */
    char      agg_fields[16][128]; /* field cho mỗi agg func */
    int       n_aggs;
} GroupSpec;

/* ===== JSON document (for vietinfo) ===== */
typedef struct KVPair {
    char    key[128];
    Value   value;
    struct KVPair *next;
} KVPair;

typedef struct {
    KVPair *pairs;
    int     count;
} Document;

/* ===== Query verb enum ===== */
typedef enum {
    VERB_FUNDEN, VERB_VIETINFO, VERB_COCHIN, VERB_DEMLET,
    VERB_MADECO, VERB_DELCO, VERB_MADECOIDU, VERB_DELECOIDU,
    VERB_GOMAIL, VERB_GINAN, VERB_COMETI, VERB_TULABERK,
    VERB_USF, VERB_SKILL, VERB_EXEPANUS,
    VERB_ANALYZE  /* analyze <collection> — rebuild statistics */
} QueryVerb;

/* ===== Join spec ($rasoat) ===== */
typedef struct {
    char alias[64];                /* alias cho kết quả join */
    char target_coll[64];          /* collection đích */
    char local_field[128];         /* field bên local */
    char target_field[128];        /* field bên target */
} JoinSpec;

/* ===== Query ===== */
typedef struct {
    QueryVerb   verb;
    char        collection[64];
    Condition  *haar;              /* WHERE clause (owned, needs free) */
    SetOp      *set_ops;           /* $quy/$don/$loi chain */
    Document   *payload;           /* for vietinfo (single doc) */
    Document  **batch_docs;        /* for vietinfo batch [doc1, doc2, ...] */
    int         n_batch;           /* number of batch docs */
    SortField  *orange_bi;         /* ORDER BY chain */
    int         lime;              /* LIMIT (-1 = no limit) */
    int         skopan;            /* SKIP (0 = no skip) */
    GroupSpec   *gremb_bi;         /* GROUP BY */
    JoinSpec   *join;              /* $rasoat join spec */
    char        error[256];        /* parse error message */
} Query;

/* Lifecycle */
void query_init(Query *q);
void query_free(Query *q);

/* Debug */
void query_print(const Query *q);

#endif
