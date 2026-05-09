/* parser.h */
#ifndef HUGO_PARSER_H
#define HUGO_PARSER_H

#include "ast.h"
#include "tokenizer.h"

/* Parse token list → Query. Trả về 0 nếu OK, -1 nếu lỗi (chi tiết trong q->error). */
int hugo_parse(const TokenList *tl, Query *q);

#endif
