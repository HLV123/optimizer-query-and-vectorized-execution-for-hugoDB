/* parser.c — HugoQL parser: Token[] → Query AST */
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const TokenList *tl;
    int pos;
} Parser;

static const Token* peek(Parser *p) { return &p->tl->tokens[p->pos]; }
static const Token* advance(Parser *p) { return &p->tl->tokens[p->pos++]; }
static int at_end(Parser *p) { return peek(p)->type == TOK_EOF; }
static int match(Parser *p, HugoTokenType t) {
    if (peek(p)->type == t) { p->pos++; return 1; }
    return 0;
}

static int is_cmp_op(HugoTokenType t) {
    return t == TOK_OP_LH  || t == TOK_OP_BH  || t == TOK_OP_LHB ||
           t == TOK_OP_BHB || t == TOK_OP_BG  || t == TOK_OP_KC  ||
           t == TOK_OP_TG  || t == TOK_OP_KTG || t == TOK_OP_XAU;
}

static int is_set_op(HugoTokenType t) {
    return t == TOK_OP_QUY || t == TOK_OP_DON || t == TOK_OP_LOI;
}

static Value parse_value(Parser *p) {
    Value v = {0};
    if (peek(p)->type == TOK_NUMBER) {
        v.type = VAL_NUM;
        v.num = peek(p)->num_val;
        advance(p);
    } else if (peek(p)->type == TOK_STRING) {
        v.type = VAL_STR;
        strncpy(v.str, peek(p)->text, sizeof(v.str)-1);
        advance(p);
    } else if (peek(p)->type == TOK_IDENT) {
        v.type = VAL_STR;
        strncpy(v.str, peek(p)->text, sizeof(v.str)-1);
        advance(p);
    } else {
        v.type = VAL_NULL;
    }
    return v;
}

/* ===== Parse haar (WHERE) clause =====
 * Grammar: haar field op value [$vand field op value]* [$vor ...]
 * Simplified: parse flat list of conditions joined by $vand/$vor.
 * $tntt has no value.
 */
static Condition* parse_condition(Parser *p) {
    if (at_end(p)) return NULL;

    /* Handle $vnot prefix */
    if (peek(p)->type == TOK_OP_VNOT) {
        advance(p);
        Condition *child = parse_condition(p);
        if (!child) return NULL;
        Condition *not_node = (Condition*)calloc(1, sizeof(Condition));
        not_node->type = COND_NOT;
        not_node->left = child;
        return not_node;
    }

    if (peek(p)->type != TOK_IDENT) return NULL;

    Condition *c = (Condition*)calloc(1, sizeof(Condition));
    strncpy(c->field, peek(p)->text, sizeof(c->field)-1);
    advance(p);

    if (peek(p)->type == TOK_OP_TNTT) {
        c->type = COND_EXISTS;
        c->op = TOK_OP_TNTT;
        advance(p);
    } else if (peek(p)->type == TOK_OP_TG || peek(p)->type == TOK_OP_KTG) {
        /* $tg / $ktg — value list or single value */
        c->type = COND_IN;
        c->op = peek(p)->type;
        advance(p);
        if (peek(p)->type == TOK_LBRACKET) {
            /* Parse [val1, val2, ...] */
            advance(p);  /* consume [ */
            while (!at_end(p) && peek(p)->type != TOK_RBRACKET && c->n_values < 64) {
                c->values[c->n_values++] = parse_value(p);
                match(p, TOK_COMMA);
            }
            match(p, TOK_RBRACKET);
        } else {
            /* Single value: $tg "admin" */
            c->values[c->n_values++] = parse_value(p);
        }
    } else if (is_cmp_op(peek(p)->type)) {
        c->type = COND_CMP;
        c->op = peek(p)->type;
        advance(p);
        c->value = parse_value(p);
    } else {
        free(c);
        return NULL;
    }
    return c;
}

static Condition* parse_haar(Parser *p) {
    Condition *first = parse_condition(p);
    if (!first) return NULL;

    Condition *root = first;
    while (!at_end(p) && (peek(p)->type == TOK_OP_VAND || peek(p)->type == TOK_OP_VOR)) {
        CondType ct = (peek(p)->type == TOK_OP_VAND) ? COND_AND : COND_OR;
        advance(p);
        Condition *right = parse_condition(p);
        if (!right) break;
        Condition *node = (Condition*)calloc(1, sizeof(Condition));
        node->type = ct;
        node->left = root;
        node->right = right;
        root = node;
    }
    return root;
}

/* ===== Parse set ops (after haar in cochin) ===== */
static SetOp* parse_set_ops(Parser *p) {
    SetOp *head = NULL, *tail = NULL;
    while (!at_end(p) && is_set_op(peek(p)->type)) {
        SetOp *op = (SetOp*)calloc(1, sizeof(SetOp));
        op->op = peek(p)->type;
        advance(p);
        if (peek(p)->type == TOK_IDENT) {
            strncpy(op->field, peek(p)->text, sizeof(op->field)-1);
            advance(p);
        }
        op->value = parse_value(p);
        if (!head) head = tail = op;
        else { tail->next = op; tail = op; }
    }
    return head;
}

/* ===== Parse document { key: value, ... } ===== */
static Document* parse_document(Parser *p) {
    if (!match(p, TOK_LBRACE)) return NULL;
    Document *doc = (Document*)calloc(1, sizeof(Document));
    KVPair *tail = NULL;
    while (!at_end(p) && peek(p)->type != TOK_RBRACE) {
        KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
        if (peek(p)->type == TOK_IDENT || peek(p)->type == TOK_STRING) {
            strncpy(kv->key, peek(p)->text, sizeof(kv->key)-1);
            advance(p);
        }
        match(p, TOK_COLON);
        kv->value = parse_value(p);
        match(p, TOK_COMMA);
        if (!doc->pairs) doc->pairs = tail = kv;
        else { tail->next = kv; tail = kv; }
        doc->count++;
    }
    match(p, TOK_RBRACE);
    return doc;
}

/* ===== Parse orange bi (ORDER BY) ===== */
static SortField* parse_orange_bi(Parser *p) {
    SortField *head = NULL, *tail = NULL;
    do {
        if (peek(p)->type != TOK_IDENT) break;
        SortField *sf = (SortField*)calloc(1, sizeof(SortField));
        strncpy(sf->field, peek(p)->text, sizeof(sf->field)-1);
        advance(p);
        sf->descending = 0;
        if (peek(p)->type == TOK_DESC) { sf->descending = 1; advance(p); }
        else if (peek(p)->type == TOK_ASC) { advance(p); }
        if (!head) head = tail = sf;
        else { tail->next = sf; tail = sf; }
    } while (match(p, TOK_COMMA));
    return head;
}

/* ===== Parse gremb bi (GROUP BY) + agg funcs ===== */
static GroupSpec* parse_gremb_bi(Parser *p) {
    GroupSpec *gs = (GroupSpec*)calloc(1, sizeof(GroupSpec));
    if (peek(p)->type == TOK_IDENT) {
        strncpy(gs->field, peek(p)->text, sizeof(gs->field)-1);
        advance(p);
    }
    while (!at_end(p) && gs->n_aggs < 16) {
        HugoTokenType ft = peek(p)->type;
        if (ft == TOK_POU || ft == TOK_SEP || ft == TOK_AWR ||
            ft == TOK_MIE || ft == TOK_MAF) {
            gs->agg_funcs[gs->n_aggs] = ft;
            advance(p);
            if (peek(p)->type == TOK_IDENT) {
                strncpy(gs->agg_fields[gs->n_aggs], peek(p)->text,
                        sizeof(gs->agg_fields[0])-1);
                advance(p);
            }
            gs->n_aggs++;
            match(p, TOK_COMMA);
        } else {
            break;
        }
    }
    return gs;
}

/* ===== Main parse ===== */
static QueryVerb tok_to_verb(HugoTokenType t) {
    switch (t) {
    case TOK_FUNDEN:    return VERB_FUNDEN;
    case TOK_VIETINFO:  return VERB_VIETINFO;
    case TOK_COCHIN:    return VERB_COCHIN;
    case TOK_DEMLET:    return VERB_DEMLET;
    case TOK_MADECO:    return VERB_MADECO;
    case TOK_DELCO:     return VERB_DELCO;
    case TOK_MADECOIDU: return VERB_MADECOIDU;
    case TOK_DELECOIDU: return VERB_DELECOIDU;
    case TOK_GOMAIL:    return VERB_GOMAIL;
    case TOK_GINAN:     return VERB_GINAN;
    case TOK_COMETI:    return VERB_COMETI;
    case TOK_TULABERK:  return VERB_TULABERK;
    case TOK_USF:       return VERB_USF;
    case TOK_SKILL:     return VERB_SKILL;
    case TOK_EXEPANUS:  return VERB_EXEPANUS;
    case TOK_ANALYZE:   return VERB_ANALYZE;
    default:            return VERB_FUNDEN;
    }
}

static int is_verb(HugoTokenType t) {
    return t >= TOK_FUNDEN && t <= TOK_ANALYZE;
}

int hugo_parse(const TokenList *tl, Query *q) {
    query_init(q);
    Parser parser = { tl, 0 };
    Parser *p = &parser;

    if (at_end(p)) {
        snprintf(q->error, sizeof(q->error), "empty query");
        return -1;
    }

    /* Expect verb */
    const Token *verb_tok = peek(p);
    if (!is_verb(verb_tok->type)) {
        snprintf(q->error, sizeof(q->error),
                 "expected verb, got %s", token_type_name(verb_tok->type));
        return -1;
    }
    q->verb = tok_to_verb(verb_tok->type);
    advance(p);

    /* Commands without collection */
    if (q->verb == VERB_GINAN || q->verb == VERB_COMETI || q->verb == VERB_TULABERK) {
        return 0;
    }
    if (q->verb == VERB_SKILL && at_end(p)) return 0;

    /* Expect collection name (or dotted field for madecoidu/delecoidu) */
    if (peek(p)->type == TOK_IDENT || (q->verb == VERB_EXEPANUS && is_verb(peek(p)->type))) {
        strncpy(q->collection, peek(p)->text, sizeof(q->collection)-1);
        advance(p);
    } else if (!at_end(p)) {
        snprintf(q->error, sizeof(q->error),
                 "expected collection name, got %s", token_type_name(peek(p)->type));
        return -1;
    }

    /* Verb-specific parsing */
    switch (q->verb) {
    case VERB_VIETINFO:
        if (peek(p)->type == TOK_LBRACKET) {
            /* Batch insert: vietinfo users [ {doc1}, {doc2}, ... ] */
            advance(p);  /* consume [ */
            int cap = 16;
            q->batch_docs = (Document**)calloc(cap, sizeof(Document*));
            q->n_batch = 0;
            while (!at_end(p) && peek(p)->type != TOK_RBRACKET) {
                Document *d = parse_document(p);
                if (!d) break;
                if (q->n_batch >= cap) {
                    cap *= 2;
                    q->batch_docs = (Document**)realloc(q->batch_docs, cap * sizeof(Document*));
                }
                q->batch_docs[q->n_batch++] = d;
                match(p, TOK_COMMA);
            }
            match(p, TOK_RBRACKET);
        } else {
            q->payload = parse_document(p);
        }
        break;
    case VERB_COCHIN:
        /* haar ... $quy/$don/$loi ... */
        if (match(p, TOK_HAAR)) q->haar = parse_haar(p);
        q->set_ops = parse_set_ops(p);
        break;
    case VERB_FUNDEN:
    case VERB_DEMLET:
    case VERB_EXEPANUS:
        if (match(p, TOK_HAAR)) q->haar = parse_haar(p);
        break;
    case VERB_GOMAIL:
        /* gomail orders [haar ...] gremb bi field agg_func field ... */
        if (match(p, TOK_HAAR)) q->haar = parse_haar(p);
        if (match(p, TOK_GREMB_BI)) q->gremb_bi = parse_gremb_bi(p);
        break;
    default:
        break;
    }

    /* Parse $rasoat join after haar (for funden) */
    if (q->verb == VERB_FUNDEN && !at_end(p) && peek(p)->type == TOK_OP_RASOAT) {
        advance(p);  /* consume $rasoat */
        q->join = (JoinSpec*)calloc(1, sizeof(JoinSpec));
        /* $rasoat alias từ target_coll on local_field $bg target_coll.field */
        if (peek(p)->type == TOK_IDENT) {
            strncpy(q->join->alias, peek(p)->text, sizeof(q->join->alias)-1);
            advance(p);
        }
        if (peek(p)->type == TOK_TU) advance(p);  /* consume 'từ' / 'tu' */
        if (peek(p)->type == TOK_IDENT) {
            strncpy(q->join->target_coll, peek(p)->text, sizeof(q->join->target_coll)-1);
            advance(p);
        }
        if (peek(p)->type == TOK_ON) advance(p);  /* consume 'on' */
        if (peek(p)->type == TOK_IDENT) {
            strncpy(q->join->local_field, peek(p)->text, sizeof(q->join->local_field)-1);
            advance(p);
        }
        if (is_cmp_op(peek(p)->type)) advance(p);  /* consume $bg etc */
        if (peek(p)->type == TOK_IDENT) {
            strncpy(q->join->target_field, peek(p)->text, sizeof(q->join->target_field)-1);
            advance(p);
        }
    }

    /* Common trailing clauses (for funden, can appear after haar) */
    while (!at_end(p)) {
        if (match(p, TOK_ORANGE_BI)) {
            q->orange_bi = parse_orange_bi(p);
        } else if (match(p, TOK_LIME)) {
            if (peek(p)->type == TOK_NUMBER) {
                q->lime = (int)peek(p)->num_val;
                advance(p);
            }
        } else if (match(p, TOK_SKOPAN)) {
            if (peek(p)->type == TOK_NUMBER) {
                q->skopan = (int)peek(p)->num_val;
                advance(p);
            }
        } else if (is_set_op(peek(p)->type) && q->verb == VERB_COCHIN) {
            /* Late set ops after haar conditions */
            SetOp *more = parse_set_ops(p);
            if (more) {
                if (!q->set_ops) q->set_ops = more;
                else {
                    SetOp *t = q->set_ops;
                    while (t->next) t = t->next;
                    t->next = more;
                }
            }
        } else {
            break;
        }
    }

    return 0;
}

/* ===== Lifecycle / debug ===== */
void query_init(Query *q) {
    memset(q, 0, sizeof(*q));
    q->lime = -1;
    q->skopan = 0;
}

static void free_cond(Condition *c) {
    if (!c) return;
    free_cond(c->left);
    free_cond(c->right);
    free(c);
}

static void free_set_ops(SetOp *s) {
    while (s) { SetOp *n = s->next; free(s); s = n; }
}

static void free_sort(SortField *s) {
    while (s) { SortField *n = s->next; free(s); s = n; }
}

static void free_doc(Document *d) {
    if (!d) return;
    KVPair *kv = d->pairs;
    while (kv) { KVPair *n = kv->next; free(kv); kv = n; }
    free(d);
}

void query_free(Query *q) {
    free_cond(q->haar);
    free_set_ops(q->set_ops);
    free_sort(q->orange_bi);
    free_doc(q->payload);
    if (q->batch_docs) {
        for (int i = 0; i < q->n_batch; i++) free_doc(q->batch_docs[i]);
        free(q->batch_docs);
    }
    if (q->gremb_bi) free(q->gremb_bi);
    if (q->join) free(q->join);
}

void query_print(const Query *q) {
    const char *vnames[] = {
        "funden","vietinfo","cochin","demlet","madeco","delco",
        "madecoidu","delecoidu","gomail","ginan","cometi","tulaberk",
        "usf","skill","exepanus"
    };
    printf("Query: verb=%s collection='%s'", vnames[q->verb], q->collection);
    if (q->lime >= 0) printf(" lime=%d", q->lime);
    if (q->skopan > 0) printf(" skopan=%d", q->skopan);
    if (q->haar) printf(" [has haar]");
    if (q->set_ops) printf(" [has set_ops]");
    if (q->payload) printf(" [has payload %d pairs]", q->payload->count);
    if (q->orange_bi) printf(" [has orange_bi]");
    if (q->gremb_bi) printf(" [has gremb_bi]");
    printf("\n");
}
