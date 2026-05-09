#ifndef LSM_MEMTABLE_H
#define LSM_MEMTABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arena.h"

/* Return codes (LSM-internal, compatible with HUGO_OK=0) */
#define LSM_OK           0
#define LSM_NOT_FOUND   -10
#define LSM_DELETED     -11
#define LSM_ERR_NOMEM   -12
#define LSM_ERR_IO      -13
#define LSM_ERR_CORRUPT -14

/* Op types */
#define LSM_OP_PUT    1
#define LSM_OP_DELETE 2

#define SKIPLIST_MAX_LEVEL  12
#define MEMTABLE_MAX_SIZE   (4 * 1024 * 1024)  /* 4 MB */

typedef struct MemtableEntry {
    uint64_t seq_num;
    uint8_t  op_type;    /* LSM_OP_PUT or LSM_OP_DELETE */
    uint16_t key_len;
    uint32_t value_len;
    uint8_t  data[1];    /* [key bytes][value bytes] - flexible, arena-owned */
} MemtableEntry;

/* Helpers: key and value pointers from entry */
static inline const uint8_t *entry_key(const MemtableEntry *e)   { return e->data; }
static inline const uint8_t *entry_value(const MemtableEntry *e) { return e->data + e->key_len; }

typedef struct SkipNode {
    MemtableEntry      *entry;    /* NULL for head sentinel */
    int                 level;
    struct SkipNode    *forward[SKIPLIST_MAX_LEVEL];
} SkipNode;

typedef struct Memtable {
    SkipNode  *head;
    int        cur_max_level;
    size_t     size_bytes;
    size_t     n_entries;
    Arena     *arena;
} Memtable;

typedef struct MemtableIterator {
    SkipNode *cur;
} MemtableIterator;

/* Lifecycle */
Memtable *memtable_create(void);
void      memtable_destroy(Memtable *mt);

/* Mutation */
int memtable_put(Memtable *mt, const void *key, size_t kl,
                 const void *val, size_t vl, uint64_t seq);
int memtable_delete(Memtable *mt, const void *key, size_t kl, uint64_t seq);

/* Query: returns LSM_OK (out_val/out_vl arena-owned, do NOT free),
 *         LSM_DELETED, or LSM_NOT_FOUND */
int memtable_get(Memtable *mt, const void *key, size_t kl,
                 const void **out_val, size_t *out_vl);

size_t memtable_size(const Memtable *mt);
bool   memtable_should_flush(const Memtable *mt);

/* Iterator */
MemtableIterator    *memtable_iter_new(Memtable *mt);
bool                 memtable_iter_valid(const MemtableIterator *it);
void                 memtable_iter_next(MemtableIterator *it);
const MemtableEntry *memtable_iter_entry(const MemtableIterator *it);
void                 memtable_iter_free(MemtableIterator *it);

#endif /* LSM_MEMTABLE_H */
