/* vec_str_intern.h — String interning for vectorized GROUP BY
 *
 * Vấn đề: GROUP BY string dùng strcmp trong inner loop → không autovectorize.
 *
 * Giải pháp: 1 pass intern strings → int32_t id[], sau đó agg loop chỉ
 * dùng id[i] làm bucket index trực tiếp — hoàn toàn integer, vectorizable.
 *
 * API:
 *   StrIntern *si = str_intern_build(arena, str_col, null_mask, n);
 *   // si->ids[i] = group id của row i  (0..n_unique-1)
 *   // si->keys[k] = string của group k
 *   // si->n_unique = số group unique
 *
 * Sau đó vec_agg_run_interned() dùng si->ids[] thay vì strcmp.
 */
#ifndef HUGO_VEC_STR_INTERN_H
#define HUGO_VEC_STR_INTERN_H

#include <stdint.h>
#include "../core/optimizer/arena.h"

#define STR_INTERN_MAX_UNIQUE  65536
#define STR_INTERN_HT_SIZE     131072  /* power-of-2, load factor ~0.5 */

typedef struct {
    int32_t  *ids;        /* ids[row] = group id, arena-alloc, size=n_rows */
    char    **keys;       /* keys[group_id] = string pointer, arena-alloc */
    int       n_unique;   /* số groups unique */
    int       n_rows;
} StrIntern;

/* Build intern table từ string column.
 * str_col[i]  : char* pointer vào KVPair (hoặc NULL)
 * null_mask[i]: 1 nếu field không tồn tại
 * n           : số rows
 */
StrIntern* str_intern_build(Arena *arena,
                             char *const *str_col,
                             const uint8_t *null_mask,
                             int n);

#endif
