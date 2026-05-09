/* vec_agg.h — Vectorized aggregation on ColBatch
 *
 * Thay AggBucket linked list (calloc mỗi group, cache miss) bằng
 * open-addressing hash table trên flat arrays trong Arena.
 *
 * Design:
 *   - Hash table gồm các flat arrays song song (key[], sum[][], count[], ...)
 *   - Probe = index vào mảng, không có pointer chase
 *   - Toàn bộ trong Arena — zero malloc trong aggregation loop
 *   - Max groups: VEC_AGG_MAX_GROUPS (power-of-2 để dùng & mask)
 *
 * Output: ColBatch mới chứa kết quả GROUP BY (mỗi group = 1 row).
 */
#ifndef HUGO_VEC_AGG_H
#define HUGO_VEC_AGG_H

#include "col_batch.h"
#include "../core/optimizer/logical_plan.h"
#include "../core/optimizer/arena.h"

#define VEC_AGG_MAX_GROUPS  65536   /* power-of-2, đủ cho queries thực tế */
#define VEC_AGG_MAX_AGGS    16

/* ===== Aggregation result table ===== */
typedef struct {
    int     n_groups;
    int     capacity;        /* power-of-2 */

    /* Group-by key per slot (numeric hoặc string) */
    int      key_is_num;
    double  *key_num;        /* arena, size=capacity */
    char   **key_str;        /* arena, size=capacity */
    uint8_t *slot_used;      /* arena, 1=occupied */

    /* Per-agg accumulators (parallel arrays, size=capacity each) */
    int      n_aggs;
    double  *sum [VEC_AGG_MAX_AGGS];   /* arena */
    double  *min_[VEC_AGG_MAX_AGGS];   /* arena */
    double  *max_[VEC_AGG_MAX_AGGS];   /* arena */
    int64_t *cnt [VEC_AGG_MAX_AGGS];   /* arena — count per group */
    uint8_t *has [VEC_AGG_MAX_AGGS];   /* arena — 1 nếu có ít nhất 1 val */

    /* Agg function types và field indices (vào ColBatch) */
    HugoTokenType agg_func [VEC_AGG_MAX_AGGS];
    int           agg_col  [VEC_AGG_MAX_AGGS]; /* col_idx trong ColBatch */
    char          out_name [VEC_AGG_MAX_AGGS][256];
    char          group_field[128];
} VecAggTable;

/* ===== API ===== */

/* Tạo VecAggTable rỗng.
 *   b          : input ColBatch (đã finalize + filter)
 *   group_field: tên field GROUP BY (phải có trong b)
 *   aggs[]     : mảng AggFunc từ PhysicalPlan
 *   n_aggs     : số agg functions
 */
VecAggTable* vec_agg_new(Arena *arena, const ColBatch *b,
                          const char *group_field,
                          const AggFunc *aggs, int n_aggs);

/* Chạy aggregation: quét toàn bộ alive rows trong b, cập nhật table.
 * Gọi sau vec_filter_apply + (tuỳ chọn) vec_filter_compact.
 */
void vec_agg_run(VecAggTable *t, const ColBatch *b);

/* Materialise kết quả vào HugoResult docs[].
 * Trả về số groups đã ghi.
 * docs_out[] phải đủ chỗ cho t->n_groups Document*.
 * Mỗi Document* được calloc — caller phải doc_free sau dùng.
 */
int vec_agg_materialize(const VecAggTable *t, Document **docs_out, int max_docs);

/* COUNT(*) đơn giản (không GROUP BY) — trả về tổng số alive rows */
int64_t vec_agg_count_star(const ColBatch *b);

/* SUM / AVG / MIN / MAX trên 1 numeric column (không GROUP BY) */
double vec_agg_sum (const ColBatch *b, int num_col_idx);
double vec_agg_avg (const ColBatch *b, int num_col_idx);
double vec_agg_min (const ColBatch *b, int num_col_idx);
double vec_agg_max (const ColBatch *b, int num_col_idx);

#endif /* HUGO_VEC_AGG_H */

/* ===== Fast GROUP BY via string interning ===== */
#include "vec_str_intern.h"

/* Drop-in replacement cho vec_agg_run() khi group-by field là STRING.
 *
 * Thay vì gọi find_or_create_str (fnv + strcmp) mỗi row trong inner loop,
 * hàm này:
 *   1. Gọi str_intern_build() — 1 pass, tạo int32_t ids[] array
 *   2. Agg loop: accumulate trực tiếp vào slot = ids[i]
 *      → không hash, không strcmp, compiler autovectorize được
 *
 * Nếu group-by là numeric → fall through vec_agg_run() bình thường.
 * Nếu n_unique > VEC_AGG_MAX_GROUPS → fall through vec_agg_run().
 */
void vec_agg_run_fast(VecAggTable *t, const ColBatch *b, Arena *arena);
