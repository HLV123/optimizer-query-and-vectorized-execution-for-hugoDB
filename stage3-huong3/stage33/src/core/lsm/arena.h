#ifndef LSM_ARENA_H
#define LSM_ARENA_H

#include <stddef.h>
#include <stdint.h>

#define ARENA_SLAB_SIZE (1024 * 1024)  /* 1 MB per slab */

typedef struct ArenaBlock {
    uint8_t           *data;
    size_t             cap;
    size_t             used;
    struct ArenaBlock *next;
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;
    size_t      total_used;
} Arena;

Arena *arena_create(void);
void   arena_destroy(Arena *a);
void  *arena_alloc(Arena *a, size_t size);  /* 8-byte aligned */

#endif /* LSM_ARENA_H */
