/* buffer_pool.h — LRU buffer pool cho pages
 *
 * Wraps PageManager. Người dùng KHÔNG đọc/ghi page trực tiếp qua pm nữa,
 * mà qua bp_get / bp_put.
 *
 * Lifecycle 1 page:
 *   bp_get(bp, pid, &frame)   → load (cache miss → evict + read disk),
 *                                 pin++, return frame để đọc/ghi data
 *   ... sửa frame->page->data ...
 *   bp_put(bp, frame, dirty)  → pin--, nếu dirty=1 thì mark dirty
 *
 * Eviction policy: LRU trên những frame có pin_count == 0.
 * Nếu evict frame đang dirty → flush xuống disk trước.
 *
 * Dùng với buffer pool nhỏ (10-64 frames) cho test stress: ép evict liên tục.
 * Production có thể 1000+ frames.
 */
#ifndef HUGO_BUFFER_POOL_H
#define HUGO_BUFFER_POOL_H

#include <stdint.h>
#include "page.h"

#define BP_OK           0
#define BP_ERR_IO      -1
#define BP_ERR_FULL    -2  /* tất cả frames đều pinned, không evict được */
#define BP_ERR_RANGE   -3
#define BP_ERR_NOMEM   -4

typedef struct BufferFrame {
    uint64_t   page_id;     /* 0 = frame trống */
    HugoPage   page;
    int        dirty;
    int        pin_count;
    /* LRU list: chỉ dùng khi pin_count == 0 */
    int        prev;        /* index trong frames[], -1 nếu head */
    int        next;        /* index trong frames[], -1 nếu tail */
    int        in_lru;      /* 1 nếu đang trong LRU list */
} BufferFrame;

typedef struct {
    PageManager  *pm;
    int           capacity;
    BufferFrame  *frames;
    int           lru_head;     /* MRU end — index frames[] */
    int           lru_tail;     /* LRU end — sẽ evict trước */
    /* Stats */
    uint64_t      stat_hits;
    uint64_t      stat_misses;
    uint64_t      stat_evictions;
    uint64_t      stat_flushes;
} BufferPool;

int  bp_init    (BufferPool *bp, PageManager *pm, int capacity);
void bp_destroy (BufferPool *bp);

/* Lấy page vào buffer pool, pin nó. Trả về frame qua *out.
 * Frame->page chứa data; sửa trên frame->page rồi gọi bp_put(dirty=1). */
int  bp_get     (BufferPool *bp, uint64_t page_id, BufferFrame **out);

/* Trả frame: pin--, mark dirty nếu dirty=1 */
int  bp_put     (BufferPool *bp, BufferFrame *frame, int dirty);

/* Tạo page mới: alloc page_id qua pm, get frame, pin, KHÔNG đọc disk
 * (vì page chưa có content). Caller fill data rồi bp_put(dirty=1). */
int  bp_new_page(BufferPool *bp, BufferFrame **out);

/* Flush tất cả dirty pages xuống disk + fsync */
int  bp_flush_all(BufferPool *bp);

/* Stats — debug */
void bp_print_stats(const BufferPool *bp);

#endif
