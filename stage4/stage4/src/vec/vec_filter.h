/* vec_filter.h — Vectorized filter on ColBatch
 *
 * Thay pe_eval_condition() row-by-row bằng các hàm loop trên flat arrays.
 * Compiler với -O3 -march=native sẽ autovectorize các loop này thành AVX2.
 *
 * Output: cập nhật b->alive[] — rows không pass predicate bị set = 0.
 * Không alloc, không free. Toàn bộ làm in-place trên ColBatch.
 */
#ifndef HUGO_VEC_FILTER_H
#define HUGO_VEC_FILTER_H

#include "col_batch.h"
#include "../query/ast.h"

/* Apply một Condition (từ PhysicalPlan) vào ColBatch.
 * Cập nhật b->alive[]: row bị loại nếu condition không thỏa.
 * Condition fields phải đã có trong ColBatch (col_batch_find_col != -1).
 * Fields không tìm thấy trong batch → row bị loại (null = fail predicate).
 */
void vec_filter_apply(ColBatch *b, const Condition *cond);

/* Compact: copy surviving rows về đầu docs[], trả về count mới.
 * Sau compact, b->alive[0..new_count-1] = 1 và b->n_rows = new_count.
 * Dùng sau khi filter xong, trước khi agg/sort để shrink working set.
 */
int vec_filter_compact(ColBatch *b);

#endif
