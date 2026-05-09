/* arena.h — Simple arena allocator for optimizer plan trees
 *
 * Plan trees có nhiều small nodes, được free toàn bộ 1 lần sau query.
 * Arena cho phép alloc nhanh và bulk-free an toàn.
 */
#ifndef HUGO_ARENA_H
#define HUGO_ARENA_H

#include <stddef.h>
#include <stdint.h>

#define ARENA_BLOCK_SIZE (64 * 1024)  /* 64 KB per block */

typedef struct ArenaBlock {
    char              *data;
    size_t             used;
    size_t             cap;
    struct ArenaBlock *next;
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;
    size_t      total_allocated;
} Arena;

Arena* arena_new(void);
void*  arena_alloc(Arena *a, size_t size);
char*  arena_strdup(Arena *a, const char *s);
void   arena_free(Arena *a);   /* free all blocks at once */

#endif
