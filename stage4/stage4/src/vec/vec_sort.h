/* vec_sort.h — Vectorized sort on ColBatch
 *
 * Thay qsort(Document**, callback) bằng sort trên int32_t perm[]:
 *   - perm[i] = original row index
 *   - Comparator compare num_data[col][perm[a]] vs num_data[col][perm[b]]
 *   - Không move Document*, chỉ permute index array
 *   - Không dùng global state (không thread-unsafe như g_pe_sort_head)
 *
 * Sau sort, truy cập rows theo thứ tự: b->docs[perm[0]], b->docs[perm[1]], ...
 *
 * Với LIMIT nhỏ: partial_sort (heap select top-k) thay full sort.
 * Threshold: nếu k <= n/8, dùng heap; ngược lại full sort.
 */
#ifndef HUGO_VEC_SORT_H
#define HUGO_VEC_SORT_H

#include "col_batch.h"
#include "../query/ast.h"
#include "../core/optimizer/arena.h"

/* Sort ColBatch theo danh sách SortField.
 * Allocate b->perm[] trong arena nếu chưa có.
 * Sau khi gọi: b->perm[0..n_alive-1] là thứ tự đúng.
 * Chỉ xét b->alive[] rows — perm chỉ chứa alive rows.
 *
 * sort_fields: linked list SortField (field, descending)
 */
void vec_sort_full(ColBatch *b, const SortField *sort_fields, Arena *arena);

/* Partial sort: chỉ đảm bảo top-k rows đầu trong perm[] đúng thứ tự.
 * Nhanh hơn full sort khi k << n (dùng max-heap size k).
 * Nếu k >= n thì tự động fall through full sort.
 */
void vec_sort_topk(ColBatch *b, const SortField *sort_fields, int k, Arena *arena);

/* Apply SKIP+LIMIT vào perm[]: điều chỉnh start/count để trả về đúng slice.
 * Trả về số rows thực sự còn lại sau skip+limit.
 * Ghi slice vào perm[0..return_value-1].
 */
int vec_sort_apply_limit(ColBatch *b, int skip, int limit);

#endif
