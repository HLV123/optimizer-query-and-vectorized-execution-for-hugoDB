#include "memtable.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- RNG (xorshift32) ---- */
static uint32_t s_rand_state;

static void rng_init(void) {
    s_rand_state = (uint32_t)(time(NULL) ^ 0xDEADBEEFu);
    if (!s_rand_state) s_rand_state = 1;
}

static uint32_t lsm_rand32(void) {
    s_rand_state ^= s_rand_state << 13;
    s_rand_state ^= s_rand_state >> 17;
    s_rand_state ^= s_rand_state << 5;
    return s_rand_state;
}

static int random_level(void) {
    int lv = 1;
    while (lv < SKIPLIST_MAX_LEVEL && (lsm_rand32() & 1)) lv++;
    return lv;
}

/* ---- Key comparison ---- */
static int cmp(const void *a, size_t al, const void *b, size_t bl) {
    size_t mn = al < bl ? al : bl;
    int r = memcmp(a, b, mn);
    if (r) return r;
    if (al < bl) return -1;
    if (al > bl) return  1;
    return 0;
}

/* ---- Node allocation from arena ---- */
static SkipNode *node_alloc(Arena *arena, int level) {
    SkipNode *n = (SkipNode *)arena_alloc(arena, sizeof(SkipNode));
    if (!n) return NULL;
    n->entry = NULL;
    n->level = level;
    for (int i = 0; i < SKIPLIST_MAX_LEVEL; i++) n->forward[i] = NULL;
    return n;
}

static MemtableEntry *entry_alloc(Arena *arena, uint8_t op, uint64_t seq,
                                   const void *key, size_t kl,
                                   const void *val, size_t vl) {
    size_t sz = sizeof(MemtableEntry) - 1 + kl + vl;
    MemtableEntry *e = (MemtableEntry *)arena_alloc(arena, sz);
    if (!e) return NULL;
    e->seq_num   = seq;
    e->op_type   = op;
    e->key_len   = (uint16_t)kl;
    e->value_len = (uint32_t)vl;
    memcpy(e->data, key, kl);
    if (vl && val) memcpy(e->data + kl, val, vl);
    return e;
}

/* ---- Lifecycle ---- */
Memtable *memtable_create(void) {
    static int rng_inited = 0;
    if (!rng_inited) { rng_init(); rng_inited = 1; }

    Arena *arena = arena_create();
    if (!arena) return NULL;

    Memtable *mt = (Memtable *)malloc(sizeof(Memtable));
    if (!mt) { arena_destroy(arena); return NULL; }

    mt->arena         = arena;
    mt->size_bytes    = 0;
    mt->n_entries     = 0;
    mt->cur_max_level = 1;
    mt->head          = node_alloc(arena, SKIPLIST_MAX_LEVEL);
    if (!mt->head) { arena_destroy(arena); free(mt); return NULL; }
    return mt;
}

void memtable_destroy(Memtable *mt) {
    if (!mt) return;
    arena_destroy(mt->arena);
    free(mt);
}

/* ---- Internal: find predecessor nodes at all levels ---- */
static void find_preds(Memtable *mt, const void *key, size_t kl,
                        SkipNode *update[SKIPLIST_MAX_LEVEL]) {
    SkipNode *cur = mt->head;
    for (int i = mt->cur_max_level - 1; i >= 0; i--) {
        while (cur->forward[i]) {
            MemtableEntry *fe = cur->forward[i]->entry;
            if (cmp(entry_key(fe), fe->key_len, key, kl) < 0)
                cur = cur->forward[i];
            else
                break;
        }
        update[i] = cur;
    }
}

/* ---- Insert a node (always inserts new node; get returns highest seq) ---- */
static int insert_node(Memtable *mt, uint8_t op, uint64_t seq,
                        const void *key, size_t kl,
                        const void *val, size_t vl) {
    SkipNode *update[SKIPLIST_MAX_LEVEL];
    find_preds(mt, key, kl, update);

    int lv = random_level();
    if (lv > mt->cur_max_level) {
        for (int i = mt->cur_max_level; i < lv; i++) update[i] = mt->head;
        mt->cur_max_level = lv;
    }

    SkipNode *node = node_alloc(mt->arena, lv);
    if (!node) return LSM_ERR_NOMEM;
    MemtableEntry *e = entry_alloc(mt->arena, op, seq, key, kl, val, vl);
    if (!e) return LSM_ERR_NOMEM;
    node->entry = e;

    for (int i = 0; i < lv; i++) {
        node->forward[i]   = update[i]->forward[i];
        update[i]->forward[i] = node;
    }

    mt->size_bytes += sizeof(SkipNode) + sizeof(MemtableEntry) - 1 + kl + vl;
    mt->n_entries++;
    return LSM_OK;
}

int memtable_put(Memtable *mt, const void *key, size_t kl,
                 const void *val, size_t vl, uint64_t seq) {
    return insert_node(mt, LSM_OP_PUT, seq, key, kl, val, vl);
}

int memtable_delete(Memtable *mt, const void *key, size_t kl, uint64_t seq) {
    return insert_node(mt, LSM_OP_DELETE, seq, key, kl, NULL, 0);
}

/* ---- Get: find the entry with the highest seq_num for the key ---- */
int memtable_get(Memtable *mt, const void *key, size_t kl,
                 const void **out_val, size_t *out_vl) {
    if (!mt || !key || !kl) return LSM_NOT_FOUND;

    /* Fast path: skip to first possible node at L0 using upper levels */
    SkipNode *cur = mt->head;
    for (int i = mt->cur_max_level - 1; i >= 1; i--) {
        while (cur->forward[i]) {
            MemtableEntry *fe = cur->forward[i]->entry;
            if (cmp(entry_key(fe), fe->key_len, key, kl) < 0)
                cur = cur->forward[i];
            else
                break;
        }
    }

    /* Scan L0 for all matching keys, keep highest seq */
    const MemtableEntry *best = NULL;
    SkipNode *n = cur->forward[0];
    while (n) {
        MemtableEntry *e = n->entry;
        int c = cmp(entry_key(e), e->key_len, key, kl);
        if (c > 0) break;
        if (c == 0 && (!best || e->seq_num > best->seq_num)) best = e;
        n = n->forward[0];
    }

    if (!best)                          return LSM_NOT_FOUND;
    if (best->op_type == LSM_OP_DELETE) return LSM_DELETED;
    if (out_val) *out_val = entry_value(best);
    if (out_vl)  *out_vl  = best->value_len;
    return LSM_OK;
}

size_t memtable_size(const Memtable *mt) { return mt ? mt->size_bytes : 0; }
bool   memtable_should_flush(const Memtable *mt) {
    return mt && mt->size_bytes >= MEMTABLE_MAX_SIZE;
}

/* ---- Iterator ---- */
MemtableIterator *memtable_iter_new(Memtable *mt) {
    MemtableIterator *it = (MemtableIterator *)malloc(sizeof(MemtableIterator));
    if (!it) return NULL;
    it->cur = mt ? mt->head->forward[0] : NULL;
    return it;
}
bool memtable_iter_valid(const MemtableIterator *it) { return it && it->cur; }
void memtable_iter_next(MemtableIterator *it) { if (it && it->cur) it->cur = it->cur->forward[0]; }
const MemtableEntry *memtable_iter_entry(const MemtableIterator *it) {
    return (it && it->cur) ? it->cur->entry : NULL;
}
void memtable_iter_free(MemtableIterator *it) { free(it); }
