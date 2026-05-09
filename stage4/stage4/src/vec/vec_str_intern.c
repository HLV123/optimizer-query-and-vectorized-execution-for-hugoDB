/* vec_str_intern.c — String interning implementation */
#include "vec_str_intern.h"
#include <string.h>
#include <stdlib.h>

/* FNV-1a cho string */
static inline uint32_t fnv_str(const char *s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* Open-addressing hashtable: slot → group_id
 * Dùng separate arrays để tránh struct padding. */
typedef struct {
    uint32_t  hash[STR_INTERN_HT_SIZE];  /* stored hash, 0=empty */
    int32_t   gid [STR_INTERN_HT_SIZE];  /* group id tại slot */
    char     *key [STR_INTERN_HT_SIZE];  /* string pointer */
} InternHT;

StrIntern* str_intern_build(Arena *arena,
                             char *const *str_col,
                             const uint8_t *null_mask,
                             int n)
{
    if (!arena || !str_col || n <= 0) return NULL;

    /* Alloc intern result */
    StrIntern *si = (StrIntern*)arena_alloc(arena, sizeof(StrIntern));
    if (!si) return NULL;
    si->ids      = (int32_t*)arena_alloc(arena, n * sizeof(int32_t));
    si->keys     = (char**)  arena_alloc(arena, STR_INTERN_MAX_UNIQUE * sizeof(char*));
    si->n_unique = 0;
    si->n_rows   = n;
    if (!si->ids || !si->keys) return NULL;

    /* Alloc hashtable on heap (large, temporary — freed before return) */
    InternHT *ht = (InternHT*)calloc(1, sizeof(InternHT));
    if (!ht) return NULL;

    uint32_t mask = STR_INTERN_HT_SIZE - 1;

    for (int i = 0; i < n; i++) {
        /* Null/missing → group id = -1 (NULL group) */
        if (null_mask && null_mask[i]) {
            si->ids[i] = -1;
            continue;
        }
        const char *s = str_col[i] ? str_col[i] : "";
        uint32_t h = fnv_str(s);
        if (h == 0) h = 1; /* 0 reserved for empty slot */

        /* Probe */
        uint32_t slot = h & mask;
        for (;;) {
            if (ht->hash[slot] == 0) {
                /* New group */
                if (si->n_unique >= STR_INTERN_MAX_UNIQUE) {
                    si->ids[i] = 0; /* overflow: map to group 0 */
                    break;
                }
                int32_t gid = si->n_unique++;
                ht->hash[slot] = h;
                ht->gid [slot] = gid;
                ht->key [slot] = (char*)s;
                si->keys[gid]  = (char*)s;
                si->ids[i]     = gid;
                break;
            }
            if (ht->hash[slot] == h && strcmp(ht->key[slot], s) == 0) {
                /* Existing group */
                si->ids[i] = ht->gid[slot];
                break;
            }
            slot = (slot + 1) & mask; /* linear probe */
        }
    }

    free(ht);
    return si;
}
