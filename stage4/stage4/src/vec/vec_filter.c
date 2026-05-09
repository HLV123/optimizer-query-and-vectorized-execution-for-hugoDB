/* vec_filter.c — Vectorized filter implementation */
#include "vec_filter.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ===== Branch-free numeric compare helpers ===== */

/* Mỗi hàm nhận double[] + threshold, cập nhật alive[] cho n rows.
 * Loop đơn giản — compiler autovectorize với -O3 -march=native. */

static void filter_num_eq(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] == val);
}

static void filter_num_ne(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] != val);
}

static void filter_num_lt(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] < val);
}

static void filter_num_gt(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] > val);
}

static void filter_num_le(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] <= val);
}

static void filter_num_ge(uint8_t *alive, const double *col,
                           const uint8_t *null_mask, double val, int n)
{
    for (int i = 0; i < n; i++)
        alive[i] &= (!null_mask[i]) & (col[i] >= val);
}

/* ===== String compare ===== */

static void filter_str_eq(uint8_t *alive, char *const *col,
                           const uint8_t *null_mask, const char *val, int n)
{
    for (int i = 0; i < n; i++) {
        if (!alive[i]) continue;
        if (null_mask[i] || !col[i]) { alive[i] = 0; continue; }
        alive[i] = (strcmp(col[i], val) == 0) ? 1 : 0;
    }
}

static void filter_str_ne(uint8_t *alive, char *const *col,
                           const uint8_t *null_mask, const char *val, int n)
{
    for (int i = 0; i < n; i++) {
        if (!alive[i]) continue;
        if (null_mask[i] || !col[i]) { alive[i] = 0; continue; }
        alive[i] = (strcmp(col[i], val) != 0) ? 1 : 0;
    }
}

static void filter_str_contains(uint8_t *alive, char *const *col,
                                  const uint8_t *null_mask, const char *val, int n)
{
    for (int i = 0; i < n; i++) {
        if (!alive[i]) continue;
        if (null_mask[i] || !col[i]) { alive[i] = 0; continue; }
        alive[i] = (strstr(col[i], val) != NULL) ? 1 : 0;
    }
}

/* ===== Dispatch single COND_CMP / COND_EXISTS ===== */

static void apply_cmp(ColBatch *b, const Condition *c)
{
    ColType ctype;
    int ci = col_batch_find_col(b, c->field, &ctype);

    if (ci < 0) {
        /* Field không có trong batch — tất cả rows fail */
        memset(b->alive, 0, b->n_rows);
        return;
    }

    ColDesc *cd = &b->cols[ci];

    if (ctype == COL_TYPE_NUM) {
        double val = (c->value.type == VAL_NUM) ? c->value.num : 0.0;
        double *col = b->num_data[cd->col_idx];
        uint8_t *nm  = b->null_mask[cd->col_idx];
        switch (c->op) {
        case TOK_OP_BG:  filter_num_eq(b->alive, col, nm, val, b->n_rows); break;
        case TOK_OP_KC:  filter_num_ne(b->alive, col, nm, val, b->n_rows); break;
        case TOK_OP_LH:  filter_num_lt(b->alive, col, nm, val, b->n_rows); break;
        case TOK_OP_BH:  filter_num_gt(b->alive, col, nm, val, b->n_rows); break;
        case TOK_OP_LHB: filter_num_le(b->alive, col, nm, val, b->n_rows); break;
        case TOK_OP_BHB: filter_num_ge(b->alive, col, nm, val, b->n_rows); break;
        default: break;
        }
    } else {
        /* string column */
        int mask_idx = COL_BATCH_MAX_COLS / 2 + cd->col_idx;
        char **col   = b->str_data[cd->col_idx];
        uint8_t *nm  = b->null_mask[mask_idx];
        const char *val = c->value.str;
        switch (c->op) {
        case TOK_OP_BG:  filter_str_eq(b->alive, col, nm, val, b->n_rows);       break;
        case TOK_OP_KC:  filter_str_ne(b->alive, col, nm, val, b->n_rows);       break;
        case TOK_OP_XAU: filter_str_contains(b->alive, col, nm, val, b->n_rows); break;
        default: break;
        }
    }
}

static void apply_exists(ColBatch *b, const Condition *c)
{
    ColType ctype;
    int ci = col_batch_find_col(b, c->field, &ctype);
    if (ci < 0) { memset(b->alive, 0, b->n_rows); return; }
    ColDesc *cd = &b->cols[ci];

    if (ctype == COL_TYPE_NUM) {
        uint8_t *nm = b->null_mask[cd->col_idx];
        for (int i = 0; i < b->n_rows; i++)
            b->alive[i] &= !nm[i];
    } else {
        int mask_idx = COL_BATCH_MAX_COLS / 2 + cd->col_idx;
        uint8_t *nm  = b->null_mask[mask_idx];
        for (int i = 0; i < b->n_rows; i++)
            b->alive[i] &= !nm[i];
    }
}

static void apply_in(ColBatch *b, const Condition *c)
{
    ColType ctype;
    int ci = col_batch_find_col(b, c->field, &ctype);
    if (ci < 0) { memset(b->alive, 0, b->n_rows); return; }
    ColDesc *cd = &b->cols[ci];
    int is_nin = (c->op == TOK_OP_KTG); /* NOT IN */

    if (ctype == COL_TYPE_NUM) {
        double *col = b->num_data[cd->col_idx];
        uint8_t *nm = b->null_mask[cd->col_idx];
        for (int i = 0; i < b->n_rows; i++) {
            if (!b->alive[i] || nm[i]) { b->alive[i] = 0; continue; }
            int found = 0;
            for (int j = 0; j < c->n_values && !found; j++)
                found = (c->values[j].type == VAL_NUM && col[i] == c->values[j].num);
            b->alive[i] = is_nin ? !found : found;
        }
    } else {
        int mask_idx = COL_BATCH_MAX_COLS / 2 + cd->col_idx;
        char **col   = b->str_data[cd->col_idx];
        uint8_t *nm  = b->null_mask[mask_idx];
        for (int i = 0; i < b->n_rows; i++) {
            if (!b->alive[i] || nm[i] || !col[i]) { b->alive[i] = 0; continue; }
            int found = 0;
            for (int j = 0; j < c->n_values && !found; j++)
                found = (c->values[j].type == VAL_STR &&
                         strcmp(col[i], c->values[j].str) == 0);
            b->alive[i] = is_nin ? !found : found;
        }
    }
}

/* ===== Recursive condition dispatch ===== */

void vec_filter_apply(ColBatch *b, const Condition *cond)
{
    if (!b || !cond) return;

    switch (cond->type) {
    case COND_CMP:
    case COND_EXISTS:
        if (cond->type == COND_EXISTS)
            apply_exists(b, cond);
        else
            apply_cmp(b, cond);
        break;

    case COND_IN:
        apply_in(b, cond);
        break;

    case COND_AND:
        /* AND: apply cả hai — thứ tự tốt hơn nếu left selective hơn */
        vec_filter_apply(b, cond->left);
        vec_filter_apply(b, cond->right);
        break;

    case COND_OR: {
        /* OR: dùng malloc cho cả 2 buffers — portable trên GCC và MSVC */
        int n = b->n_rows;
        uint8_t *saved       = (uint8_t*)malloc(n);
        uint8_t *left_result = (uint8_t*)malloc(n);
        if (!saved || !left_result) {
            free(saved); free(left_result);
            vec_filter_apply(b, cond->left);
            break;
        }
        /* Save current alive state */
        memcpy(saved, b->alive, n);

        /* Apply left, save result */
        vec_filter_apply(b, cond->left);
        memcpy(left_result, b->alive, n);

        /* Restore, apply right */
        memcpy(b->alive, saved, n);
        vec_filter_apply(b, cond->right);

        /* OR merge */
        for (int i = 0; i < n; i++)
            b->alive[i] = left_result[i] | b->alive[i];

        free(saved);
        free(left_result);
        break;
    }

    case COND_NOT: {
        /* NOT: apply child, flip alive */
        vec_filter_apply(b, cond->left);
        for (int i = 0; i < b->n_rows; i++)
            b->alive[i] ^= 1;
        break;
    }
    }
}

int vec_filter_compact(ColBatch *b)
{
    if (!b) return 0;
    int write = 0;
    for (int read = 0; read < b->n_rows; read++) {
        if (!b->alive[read]) continue;
        if (write != read) {
            /* Move doc pointer */
            b->docs[write] = b->docs[read];
            /* Move all column data */
            for (int c = 0; c < b->n_num_cols; c++) {
                b->num_data[c][write]  = b->num_data[c][read];
                b->null_mask[c][write] = b->null_mask[c][read];
            }
            for (int c = 0; c < b->n_str_cols; c++) {
                b->str_data[c][write] = b->str_data[c][read];
                int mi = COL_BATCH_MAX_COLS / 2 + c;
                b->null_mask[mi][write] = b->null_mask[mi][read];
            }
            b->alive[write] = 1;
        }
        write++;
    }
    b->n_rows = write;
    return write;
}
