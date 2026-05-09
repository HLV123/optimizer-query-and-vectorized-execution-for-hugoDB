/* buffer_pool.c — LRU buffer pool implementation
 *
 * Linear scan để lookup page_id → frame index. Đủ nhanh với capacity nhỏ
 * (≤ 256 frames). Capacity lớn hơn nên dùng hash table (TODO).
 *
 * LRU list: doubly-linked, head = MRU, tail = LRU.
 * Chỉ frame có pin_count == 0 mới nằm trong LRU list.
 * Khi pin++ một frame trong LRU → remove khỏi LRU.
 * Khi pin trở về 0 → push vào MRU end.
 */
#include "buffer_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== LRU list helpers ===== */
static void lru_remove(BufferPool *bp, int idx) {
    BufferFrame *f = &bp->frames[idx];
    if (!f->in_lru) return;
    if (f->prev >= 0) bp->frames[f->prev].next = f->next;
    else              bp->lru_head = f->next;
    if (f->next >= 0) bp->frames[f->next].prev = f->prev;
    else              bp->lru_tail = f->prev;
    f->prev = f->next = -1;
    f->in_lru = 0;
}

static void lru_push_front(BufferPool *bp, int idx) {
    BufferFrame *f = &bp->frames[idx];
    if (f->in_lru) lru_remove(bp, idx);
    f->prev = -1;
    f->next = bp->lru_head;
    if (bp->lru_head >= 0) bp->frames[bp->lru_head].prev = idx;
    bp->lru_head = idx;
    if (bp->lru_tail < 0) bp->lru_tail = idx;
    f->in_lru = 1;
}

/* ===== Init / destroy ===== */
int bp_init(BufferPool *bp, PageManager *pm, int capacity) {
    if (capacity <= 0) return BP_ERR_RANGE;
    bp->pm = pm;
    bp->capacity = capacity;
    bp->frames = (BufferFrame*)calloc(capacity, sizeof(BufferFrame));
    if (!bp->frames) return BP_ERR_NOMEM;
    for (int i = 0; i < capacity; i++) {
        bp->frames[i].page_id = 0;
        bp->frames[i].pin_count = 0;
        bp->frames[i].dirty = 0;
        bp->frames[i].prev = bp->frames[i].next = -1;
        bp->frames[i].in_lru = 0;
    }
    bp->lru_head = bp->lru_tail = -1;
    bp->stat_hits = bp->stat_misses = 0;
    bp->stat_evictions = bp->stat_flushes = 0;
    return BP_OK;
}

void bp_destroy(BufferPool *bp) {
    if (!bp || !bp->frames) return;
    free(bp->frames);
    bp->frames = NULL;
}

/* ===== Lookup ===== */
static int find_frame(const BufferPool *bp, uint64_t page_id) {
    /* page_id = 0 nghĩa là frame trống → không match */
    if (page_id == 0) return -1;
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page_id == page_id) return i;
    }
    return -1;
}

static int find_free_frame(const BufferPool *bp) {
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page_id == 0) return i;
    }
    return -1;
}

/* Flush 1 frame xuống disk nếu dirty. Không thay đổi pin/page_id. */
static int flush_frame(BufferPool *bp, int idx) {
    BufferFrame *f = &bp->frames[idx];
    if (!f->dirty || f->page_id == 0) return BP_OK;
    if (pm_write_page(bp->pm, &f->page) != PG_OK) return BP_ERR_IO;
    f->dirty = 0;
    bp->stat_flushes++;
    return BP_OK;
}

/* Evict frame LRU không pinned. Trả về index, hoặc -1 nếu không có. */
static int evict_one(BufferPool *bp) {
    /* LRU tail là frame ít dùng nhất, không pin */
    int idx = bp->lru_tail;
    if (idx < 0) return -1;

    BufferFrame *f = &bp->frames[idx];
    /* Sanity */
    if (f->pin_count != 0) return -1;

    if (flush_frame(bp, idx) != BP_OK) return -1;
    lru_remove(bp, idx);
    f->page_id = 0;  /* đánh dấu frame trống */
    bp->stat_evictions++;
    return idx;
}

/* Tìm hoặc tạo slot trống cho page mới (chưa load) */
static int acquire_slot(BufferPool *bp) {
    int idx = find_free_frame(bp);
    if (idx >= 0) return idx;
    return evict_one(bp);
}

/* ===== Public API ===== */
int bp_get(BufferPool *bp, uint64_t page_id, BufferFrame **out) {
    if (page_id == 0) return BP_ERR_RANGE;

    int idx = find_frame(bp, page_id);
    if (idx >= 0) {
        BufferFrame *f = &bp->frames[idx];
        if (f->pin_count == 0) lru_remove(bp, idx);
        f->pin_count++;
        bp->stat_hits++;
        *out = f;
        return BP_OK;
    }

    /* Cache miss: load từ disk */
    bp->stat_misses++;
    idx = acquire_slot(bp);
    if (idx < 0) return BP_ERR_FULL;

    BufferFrame *f = &bp->frames[idx];
    if (pm_read_page(bp->pm, page_id, &f->page) != PG_OK) {
        f->page_id = 0;
        return BP_ERR_IO;
    }
    f->page_id = page_id;
    f->dirty = 0;
    f->pin_count = 1;
    /* Không thêm vào LRU vì đang pinned */
    *out = f;
    return BP_OK;
}

int bp_put(BufferPool *bp, BufferFrame *frame, int dirty) {
    if (!frame || frame->page_id == 0) return BP_ERR_RANGE;
    if (frame->pin_count <= 0) return BP_ERR_RANGE;

    if (dirty) frame->dirty = 1;
    frame->pin_count--;
    if (frame->pin_count == 0) {
        /* Đẩy vào MRU end */
        int idx = (int)(frame - bp->frames);
        lru_push_front(bp, idx);
    }
    return BP_OK;
}

int bp_new_page(BufferPool *bp, BufferFrame **out) {
    uint64_t pid;
    if (pm_alloc_page(bp->pm, &pid) != PG_OK) return BP_ERR_IO;

    int idx = acquire_slot(bp);
    if (idx < 0) return BP_ERR_FULL;

    BufferFrame *f = &bp->frames[idx];
    memset(&f->page, 0, sizeof(f->page));
    f->page.page_id = (uint32_t)pid;
    f->page_id = pid;
    f->dirty = 1;       /* page mới luôn dirty (cần ghi xuống disk) */
    f->pin_count = 1;
    bp->stat_misses++;  /* coi như miss */
    *out = f;
    return BP_OK;
}

int bp_flush_all(BufferPool *bp) {
    for (int i = 0; i < bp->capacity; i++) {
        if (bp->frames[i].page_id != 0 && bp->frames[i].dirty) {
            if (flush_frame(bp, i) != BP_OK) return BP_ERR_IO;
        }
    }
    if (hugo_sync(bp->pm->file) != HUGO_OK) return BP_ERR_IO;
    return BP_OK;
}

void bp_print_stats(const BufferPool *bp) {
    uint64_t total = bp->stat_hits + bp->stat_misses;
    double hit_rate = total ? (100.0 * bp->stat_hits / total) : 0.0;
    printf("BP stats: capacity=%d hits=%llu misses=%llu (hit_rate=%.1f%%) "
           "evictions=%llu flushes=%llu\n",
           bp->capacity,
           (unsigned long long)bp->stat_hits,
           (unsigned long long)bp->stat_misses,
           hit_rate,
           (unsigned long long)bp->stat_evictions,
           (unsigned long long)bp->stat_flushes);
}
