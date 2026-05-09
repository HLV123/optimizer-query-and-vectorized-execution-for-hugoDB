/* arena.c — Arena allocator implementation */
#include "arena.h"
#include <stdlib.h>
#include <string.h>

Arena* arena_new(void) {
    Arena *a = (Arena*)calloc(1, sizeof(Arena));
    return a;
}

static ArenaBlock* block_new(size_t min_size) {
    size_t cap = min_size > ARENA_BLOCK_SIZE ? min_size : ARENA_BLOCK_SIZE;
    ArenaBlock *b = (ArenaBlock*)malloc(sizeof(ArenaBlock));
    if (!b) return NULL;
    b->data = (char*)malloc(cap);
    if (!b->data) { free(b); return NULL; }
    b->used = 0;
    b->cap  = cap;
    b->next = NULL;
    return b;
}

void* arena_alloc(Arena *a, size_t size) {
    if (!a) return NULL;
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    /* Try current block */
    if (a->head && (a->head->used + size) <= a->head->cap) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += size;
        a->total_allocated += size;
        return ptr;
    }

    /* Allocate new block */
    ArenaBlock *b = block_new(size);
    if (!b) return NULL;
    b->next = a->head;
    a->head = b;
    b->used = size;
    a->total_allocated += size;
    return b->data;
}

char* arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = (char*)arena_alloc(a, len);
    if (p) memcpy(p, s, len);
    return p;
}

void arena_free(Arena *a) {
    if (!a) return;
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b->data);
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total_allocated = 0;
    free(a);
}
