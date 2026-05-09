/* vec_sort.c — Vectorized sort implementation */
#include "vec_sort.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===== Sort context (passed by pointer — no global state) ===== */
typedef struct {
    const ColBatch   *b;
    const SortField  *sort_fields;
    /* Pre-resolved column indices để tránh col_batch_find_col trong comparator */
    int   n_keys;
    int   col_idx   [16]; /* col index trong num/str data */
    int   is_num    [16]; /* 1=numeric, 0=string */
    int   descending[16];
} SortCtx;

/* ===== Row comparison ===== */
static int cmp_rows(const SortCtx *ctx, int a, int b) {
    for (int k = 0; k < ctx->n_keys; k++) {
        int ci   = ctx->col_idx[k];
        int desc = ctx->descending[k];
        int c;

        if (ctx->is_num[k]) {
            double va = ctx->b->num_data[ci][a];
            double vb = ctx->b->num_data[ci][b];
            /* null handling: null sorts last */
            int na = ctx->b->null_mask[ci][a];
            int nb = ctx->b->null_mask[ci][b];
            if (na && nb) continue;
            if (na) return desc ? -1 : 1;
            if (nb) return desc ? 1 : -1;
            c = (va > vb) ? 1 : (va < vb) ? -1 : 0;
        } else {
            int mi = COL_BATCH_MAX_COLS / 2 + ci;
            const char *sa = ctx->b->str_data[ci][a];
            const char *sb = ctx->b->str_data[ci][b];
            int na = ctx->b->null_mask[mi][a];
            int nb = ctx->b->null_mask[mi][b];
            if (na && nb) continue;
            if (na) return desc ? -1 : 1;
            if (nb) return desc ? 1 : -1;
            if (!sa) sa = "";
            if (!sb) sb = "";
            c = strcmp(sa, sb);
        }

        if (c != 0) return desc ? -c : c;
    }
    return 0;
}

/* ===== Build SortCtx từ SortField linked list ===== */
static void build_ctx(SortCtx *ctx, const ColBatch *b, const SortField *sf)
{
    ctx->b           = b;
    ctx->sort_fields = sf;
    ctx->n_keys      = 0;

    for (const SortField *s = sf; s && ctx->n_keys < 16; s = s->next) {
        ColType ctype;
        int ci = col_batch_find_col(b, s->field, &ctype);
        if (ci < 0) continue; /* field không có trong batch — skip key */
        int real_idx = b->cols[ci].col_idx;
        ctx->col_idx   [ctx->n_keys] = real_idx;
        ctx->is_num    [ctx->n_keys] = (ctype == COL_TYPE_NUM);
        ctx->descending[ctx->n_keys] = s->descending;
        ctx->n_keys++;
    }
}

/* ===== Introsort (quicksort + heapsort fallback) trên perm[] ===== */

/* Median-of-3 pivot */
static inline int med3(const SortCtx *ctx, int32_t *p, int a, int b, int c) {
    int ab = cmp_rows(ctx, p[a], p[b]);
    int bc = cmp_rows(ctx, p[b], p[c]);
    int ac = cmp_rows(ctx, p[a], p[c]);
    if (ab <= 0 && bc <= 0) return b;
    if (ac <= 0 && bc >= 0) return c;
    return a;
}

static inline void swap_perm(int32_t *p, int a, int b) {
    int32_t t = p[a]; p[a] = p[b]; p[b] = t;
}

/* Insertion sort pro small slices */
static void isort(const SortCtx *ctx, int32_t *p, int n) {
    for (int i = 1; i < n; i++) {
        int32_t key = p[i];
        int j = i - 1;
        while (j >= 0 && cmp_rows(ctx, p[j], key) > 0) {
            p[j+1] = p[j]; j--;
        }
        p[j+1] = key;
    }
}

/* Heapify down */
static void sift_down(const SortCtx *ctx, int32_t *p, int i, int n) {
    for (;;) {
        int largest = i, l = 2*i+1, r = 2*i+2;
        if (l < n && cmp_rows(ctx, p[l], p[largest]) > 0) largest = l;
        if (r < n && cmp_rows(ctx, p[r], p[largest]) > 0) largest = r;
        if (largest == i) break;
        swap_perm(p, i, largest);
        i = largest;
    }
}

static void heapsort(const SortCtx *ctx, int32_t *p, int n) {
    for (int i = n/2-1; i >= 0; i--) sift_down(ctx, p, i, n);
    for (int i = n-1; i > 0; i--) { swap_perm(p, 0, i); sift_down(ctx, p, 0, i); }
}

/* Introsort: quicksort với depth limit, fallback heapsort */
static void introsort(const SortCtx *ctx, int32_t *p, int n, int depth_limit) {
    while (n > 16) {
        if (depth_limit == 0) { heapsort(ctx, p, n); return; }
        depth_limit--;

        /* Median-of-3 pivot */
        int mid = n / 2;
        int pi  = med3(ctx, p, 0, mid, n-1);
        swap_perm(p, pi, n-1);
        int32_t pivot = p[n-1];

        /* Partition */
        int lo = 0, hi = n-2;
        while (lo <= hi) {
            while (lo <= hi && cmp_rows(ctx, p[lo], pivot) < 0) lo++;
            while (lo <= hi && cmp_rows(ctx, p[hi], pivot) > 0) hi--;
            if (lo <= hi) { swap_perm(p, lo, hi); lo++; hi--; }
        }
        /* Place pivot */
        swap_perm(p, lo, n-1);

        /* Recurse on smaller half, iterate on larger */
        if (lo < n - lo - 1) {
            introsort(ctx, p,      lo,         depth_limit);
            p += lo + 1; n -= lo + 1;
        } else {
            introsort(ctx, p+lo+1, n-lo-1,     depth_limit);
            n = lo;
        }
    }
    if (n > 1) isort(ctx, p, n);
}

/* Compute floor(log2(n)) * 2 for depth limit */
static int ilog2x2(int n) {
    int d = 0; while (n > 1) { n >>= 1; d++; } return d * 2;
}

/* ===== Build perm[] of alive rows ===== */
static int32_t* build_perm(ColBatch *b, Arena *arena, int *out_n)
{
    /* Count alive */
    int n = 0;
    for (int i = 0; i < b->n_rows; i++) n += b->alive[i];

    if (b->perm) {
        /* Reuse if already allocated (capacity = n_rows) */
    } else {
        b->perm = (int32_t*)arena_alloc(arena, b->n_rows * sizeof(int32_t));
    }
    if (!b->perm) { *out_n = 0; return NULL; }

    /* Fill with alive indices */
    int w = 0;
    for (int i = 0; i < b->n_rows; i++)
        if (b->alive[i]) b->perm[w++] = i;
    *out_n = w;
    return b->perm;
}

/* ===== Public API ===== */

void vec_sort_full(ColBatch *b, const SortField *sort_fields, Arena *arena)
{
    if (!b || !sort_fields) return;

    SortCtx ctx;
    build_ctx(&ctx, b, sort_fields);
    if (ctx.n_keys == 0) return;

    int n;
    int32_t *perm = build_perm(b, arena, &n);
    if (!perm || n <= 1) return;

    introsort(&ctx, perm, n, ilog2x2(n));
}

/* ===== Max-heap for top-k (min-heap of size k, keeps k largest) ===== */
/* Để lấy top-k SMALLEST (ASC order), dùng max-heap kích thước k:
 * nếu phần tử mới < heap[0] thì thay thế và sift down. */

typedef struct { const SortCtx *ctx; int32_t *heap; int k; } HeapCtx;

static void heap_sift_down_max(const SortCtx *ctx, int32_t *h, int i, int n) {
    for (;;) {
        int largest = i, l=2*i+1, r=2*i+2;
        if (l<n && cmp_rows(ctx, h[l], h[largest]) > 0) largest=l;
        if (r<n && cmp_rows(ctx, h[r], h[largest]) > 0) largest=r;
        if (largest==i) break;
        int32_t t=h[i]; h[i]=h[largest]; h[largest]=t;
        i=largest;
    }
}

void vec_sort_topk(ColBatch *b, const SortField *sort_fields, int k, Arena *arena)
{
    if (!b || !sort_fields || k <= 0) return;

    int n;
    int32_t *perm = build_perm(b, arena, &n);
    if (!perm || n <= 1) return;

    /* If k >= n, just do full sort */
    if (k >= n) { vec_sort_full(b, sort_fields, arena); return; }

    SortCtx ctx;
    build_ctx(&ctx, b, sort_fields);
    if (ctx.n_keys == 0) return;

    /* Build max-heap of first k elements */
    int32_t *heap = (int32_t*)arena_alloc(arena, k * sizeof(int32_t));
    if (!heap) { vec_sort_full(b, sort_fields, arena); return; }
    memcpy(heap, perm, k * sizeof(int32_t));
    for (int i = k/2-1; i >= 0; i--)
        heap_sift_down_max(&ctx, heap, i, k);

    /* For each remaining element: if smaller than max, replace */
    for (int i = k; i < n; i++) {
        if (cmp_rows(&ctx, perm[i], heap[0]) < 0) {
            heap[0] = perm[i];
            heap_sift_down_max(&ctx, heap, 0, k);
        }
    }

    /* Sort the k winners (ascending) via heapsort */
    /* Invert comparator for ascending: heapsort heap as min-heap */
    /* Easiest: just run introsort on the k winners */
    introsort(&ctx, heap, k, ilog2x2(k));

    /* Write back into perm */
    memcpy(perm, heap, k * sizeof(int32_t));
    /* perm[0..k-1] = sorted top-k; rest is garbage but vec_sort_apply_limit
     * will only use [skip..skip+limit-1] */
}

int vec_sort_apply_limit(ColBatch *b, int skip, int limit)
{
    if (!b || !b->perm) return 0;

    /* Count alive in perm */
    int n = 0;
    for (int i = 0; i < b->n_rows; i++) n += b->alive[i];

    if (skip < 0) skip = 0;
    if (skip >= n) return 0;
    n -= skip;

    if (limit >= 0 && n > limit) n = limit;

    /* Shift perm if skip > 0 */
    if (skip > 0)
        memmove(b->perm, b->perm + skip, n * sizeof(int32_t));

    return n;
}
