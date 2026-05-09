/* col_batch.h — Columnar batch for vectorized execution
 *
 * Thay vì gọi doc_get_field() mỗi row trong execution loop (O(n) linked-list
 * walk mỗi lần), ColBatch extract một lần tất cả fields cần thiết ra flat arrays:
 *
 *   double  num_cols[MAX_COLS][N]   — numeric fields
 *   char    str_cols[MAX_COLS][N][256] — string fields (hoặc pointer array)
 *   uint8_t null_mask[MAX_COLS][N] — 1 nếu field không tồn tại
 *
 * Sau khi có ColBatch, filter/agg/sort chỉ dùng số index, không cần Document*.
 * Document* chỉ được rebuild ở bước cuối với rows đã survive filter.
 *
 * Memory: toàn bộ alloc qua Arena* — reset 1 lần sau query, zero malloc trong loop.
 */
#ifndef HUGO_COL_BATCH_H
#define HUGO_COL_BATCH_H

#include <stdint.h>
#include <stddef.h>
#include "../query/ast.h"
#include "../core/optimizer/arena.h"

/* ===== Limits ===== */
#define COL_BATCH_MAX_ROWS  200000
#define COL_BATCH_MAX_COLS  32
#define COL_STR_WIDTH       256   /* max string width, same as Value.str */

/* ===== Column descriptor ===== */
typedef enum {
    COL_TYPE_NUM = 0,
    COL_TYPE_STR = 1,
} ColType;

typedef struct {
    char    field[128];   /* field name, e.g. "age" hay "address.city" */
    ColType type;
    int     col_idx;      /* index vào num_data[] hoặc str_data[] */
} ColDesc;

/* ===== The batch itself ===== */
typedef struct {
    /* Row count */
    int n_rows;

    /* Column descriptors */
    ColDesc cols[COL_BATCH_MAX_COLS];
    int     n_num_cols;   /* số numeric columns */
    int     n_str_cols;   /* số string columns */

    /* Flat numeric data: num_data[col_idx][row] */
    double *num_data[COL_BATCH_MAX_COLS];  /* arena-allocated, size = n_rows */

    /* Flat string data: str_data[col_idx][row] — pointer vào arena */
    char  **str_data[COL_BATCH_MAX_COLS];  /* arena-allocated array of char* */

    /* Null mask: null_mask[col_idx][row] = 1 nếu field không tồn tại */
    uint8_t *null_mask[COL_BATCH_MAX_COLS]; /* arena-allocated, size = n_rows */

    /* Original document pointers (borrowed — ColBatch không own) */
    Document **docs;   /* docs[row] = Document* gốc, size = n_rows */

    /* Alive bitmap: alive[row] = 1 nếu row chưa bị filter loại */
    uint8_t  *alive;   /* arena-allocated, size = n_rows */

    /* Sort permutation: perm[i] = index vào docs[] theo thứ tự đã sort */
    int32_t  *perm;    /* arena-allocated, size = n_rows, NULL nếu chưa sort */
} ColBatch;

/* ===== API ===== */

/* Tạo ColBatch rỗng, dùng arena cho tất cả alloc.
 * fields[]  : danh sách field names cần extract
 * types[]   : COL_TYPE_NUM hoặc COL_TYPE_STR cho mỗi field
 * n_fields  : số fields
 * cap       : số rows tối đa (thường là n docs trong collection)
 */
ColBatch* col_batch_new(Arena *arena,
                        const char **fields, const ColType *types, int n_fields,
                        int cap);

/* Extract tất cả fields từ một Document vào row `row_idx`.
 * Gọi một lần cho mỗi document trong scan loop.
 */
void col_batch_add_doc(ColBatch *b, Document *doc, int row_idx);

/* Finalize sau khi add xong tất cả docs: set n_rows, init alive[] = 1 */
void col_batch_finalize(ColBatch *b, int n_rows);

/* Trả về column index cho field name, -1 nếu không tìm thấy */
int col_batch_find_col(const ColBatch *b, const char *field, ColType *type_out);

/* Đếm số rows còn alive (sau filter) */
int col_batch_alive_count(const ColBatch *b);

#endif /* HUGO_COL_BATCH_H */
