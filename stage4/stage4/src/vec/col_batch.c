/* col_batch.c — ColBatch implementation */
#include "col_batch.h"
#include "../core/collection.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

ColBatch* col_batch_new(Arena *arena,
                        const char **fields, const ColType *types, int n_fields,
                        int cap)
{
    if (!arena || n_fields <= 0 || cap <= 0) return NULL;
    if (cap > COL_BATCH_MAX_ROWS) cap = COL_BATCH_MAX_ROWS;
    if (n_fields > COL_BATCH_MAX_COLS) n_fields = COL_BATCH_MAX_COLS;

    ColBatch *b = (ColBatch*)arena_alloc(arena, sizeof(ColBatch));
    if (!b) return NULL;
    memset(b, 0, sizeof(ColBatch));

    /* docs[] array — pointers borrowed từ scan, không owned */
    b->docs  = (Document**)arena_alloc(arena, cap * sizeof(Document*));
    b->alive = (uint8_t*)arena_alloc(arena, cap * sizeof(uint8_t));
    b->perm  = NULL; /* allocated khi cần sort */
    if (!b->docs || !b->alive) return NULL;

    int num_idx = 0, str_idx = 0;

    for (int i = 0; i < n_fields; i++) {
        ColDesc *cd = &b->cols[i];
        strncpy(cd->field, fields[i], sizeof(cd->field) - 1);
        cd->type = types[i];

        if (types[i] == COL_TYPE_NUM) {
            cd->col_idx = num_idx;
            b->num_data[num_idx]  = (double*)arena_alloc(arena, cap * sizeof(double));
            b->null_mask[num_idx] = (uint8_t*)arena_alloc(arena, cap * sizeof(uint8_t));
            if (!b->num_data[num_idx] || !b->null_mask[num_idx]) return NULL;
            memset(b->null_mask[num_idx], 0, cap * sizeof(uint8_t));
            num_idx++;
            b->n_num_cols = num_idx;
        } else {
            cd->col_idx = str_idx;
            /* array of char* — mỗi pointer trỏ vào arena string */
            b->str_data[str_idx]  = (char**)arena_alloc(arena, cap * sizeof(char*));
            /* reuse null_mask array index = str_idx + MAX_COLS/2 để tách riêng */
            int mask_idx = COL_BATCH_MAX_COLS / 2 + str_idx;
            b->null_mask[mask_idx] = (uint8_t*)arena_alloc(arena, cap * sizeof(uint8_t));
            if (!b->str_data[str_idx] || !b->null_mask[mask_idx]) return NULL;
            memset(b->null_mask[mask_idx], 0, cap * sizeof(uint8_t));
            str_idx++;
            b->n_str_cols = str_idx;
        }
    }

    return b;
}

void col_batch_add_doc(ColBatch *b, Document *doc, int row_idx)
{
    if (!b || !doc || row_idx < 0 || row_idx >= COL_BATCH_MAX_ROWS) return;

    /* Store document pointer */
    b->docs[row_idx] = doc;

    /* Extract each registered column */
    for (int i = 0; i < b->n_num_cols + b->n_str_cols; i++) {
        if (i >= COL_BATCH_MAX_COLS) break;
        ColDesc *cd = &b->cols[i];
        Value v;

        if (doc_get_field(doc, cd->field, &v) != 0) {
            /* Field không tồn tại — mark null */
            if (cd->type == COL_TYPE_NUM) {
                b->null_mask[cd->col_idx][row_idx] = 1;
                b->num_data[cd->col_idx][row_idx]  = 0.0;
            } else {
                int mask_idx = COL_BATCH_MAX_COLS / 2 + cd->col_idx;
                b->null_mask[mask_idx][row_idx] = 1;
                b->str_data[cd->col_idx][row_idx] = NULL;
            }
            continue;
        }

        if (cd->type == COL_TYPE_NUM) {
            b->num_data[cd->col_idx][row_idx] = (v.type == VAL_NUM) ? v.num : 0.0;
            b->null_mask[cd->col_idx][row_idx] = (v.type == VAL_NULL) ? 1 : 0;
        } else {
            /* Str: lấy pointer TRỰC TIẾP từ KVPair trong doc (stable).
             * v.str là copy trên stack — bị overwrite sau loop, không dùng được. */
            int mask_idx = COL_BATCH_MAX_COLS / 2 + cd->col_idx;
            char *str_ptr = NULL;
            for (KVPair *kv = doc->pairs; kv; kv = kv->next) {
                if (strcmp(kv->key, cd->field) == 0 && kv->value.type == VAL_STR) {
                    str_ptr = kv->value.str;
                    break;
                }
            }
            b->str_data[cd->col_idx][row_idx] = str_ptr;
            b->null_mask[mask_idx][row_idx]    = (str_ptr == NULL) ? 1 : 0;
        }
    }
}

void col_batch_finalize(ColBatch *b, int n_rows)
{
    if (!b) return;
    b->n_rows = n_rows;
    /* Init tất cả rows là alive */
    memset(b->alive, 1, n_rows * sizeof(uint8_t));
}

int col_batch_find_col(const ColBatch *b, const char *field, ColType *type_out)
{
    if (!b || !field) return -1;
    int total = b->n_num_cols + b->n_str_cols;
    for (int i = 0; i < total && i < COL_BATCH_MAX_COLS; i++) {
        if (strcmp(b->cols[i].field, field) == 0) {
            if (type_out) *type_out = b->cols[i].type;
            return i; /* index vào cols[], không phải col_idx */
        }
    }
    return -1;
}

int col_batch_alive_count(const ColBatch *b)
{
    if (!b) return 0;
    int c = 0;
    for (int i = 0; i < b->n_rows; i++)
        c += b->alive[i];
    return c;
}
