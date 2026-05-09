/* executor.h — HugoQL executor
 *
 * Input: Query (AST from parser)
 * Output: Result = list of Documents + status + count
 */
#ifndef HUGO_EXECUTOR_H
#define HUGO_EXECUTOR_H

#include "../query/ast.h"
#include "collection.h"

#define MAX_RESULT_DOCS 10000

typedef struct {
    int         ok;                                /* 1 = ok, 0 = err */
    int         count;                             /* số document trả về hoặc ảnh hưởng */
    Document   *docs[MAX_RESULT_DOCS];             /* danh sách (owned by collection, KHÔNG free) */
    char        err_code[64];
    char        err_msg[256];
    char        info[256];                         /* text info cho success (optional) */
} HugoResult;

void result_init(HugoResult *r);

/* Execute query against database. Fills result. Return 0 (không dùng code này,
 * luôn xem result.ok). */
int hugo_execute(HugoDatabase *db, const Query *q, HugoResult *r);

/* Format result cho output dòng lệnh (theo spec "ok/err"). Ghi vào stdout. */
void result_print(const HugoResult *r);

#endif
