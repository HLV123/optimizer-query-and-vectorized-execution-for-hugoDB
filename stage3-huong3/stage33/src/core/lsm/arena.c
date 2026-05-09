#include "arena.h"
#include <stdlib.h>
#include <string.h>

Arena *arena_create(void) {
    Arena *a = (Arena *)calloc(1, sizeof(Arena));
    if (!a) return NULL;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    if (!b) { free(a); return NULL; }
    b->data = (uint8_t *)malloc(ARENA_SLAB_SIZE);
    if (!b->data) { free(b); free(a); return NULL; }
    b->cap  = ARENA_SLAB_SIZE;
    b->used = 0;
    b->next = NULL;
    a->head = b;
    a->total_used = 0;
    return a;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    free(a);
}

void *arena_alloc(Arena *a, size_t size) {
    if (!a || size == 0) return NULL;
    /* align to 8 bytes */
    size = (size + 7u) & ~(size_t)7u;

    if (a->head->used + size <= a->head->cap) {
        void *p = a->head->data + a->head->used;
        a->head->used  += size;
        a->total_used  += size;
        return p;
    }
    /* new slab */
    size_t cap = size > ARENA_SLAB_SIZE ? size : ARENA_SLAB_SIZE;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    if (!b) return NULL;
    b->data = (uint8_t *)malloc(cap);
    if (!b->data) { free(b); return NULL; }
    b->cap  = cap;
    b->used = size;
    b->next = a->head;
    a->head = b;
    a->total_used += size;
    return b->data;
}
