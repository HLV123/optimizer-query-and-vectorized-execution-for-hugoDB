/* vec_scan_cache.c — Scan cache implementation */
#include "vec_scan_cache.h"
#include "vec_bulk_scan.h"
#include "../core/collection.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

uint64_t scan_cache_fingerprint(const DiskColl *c) {
    return (c->count << 20) ^ c->next_id ^ (c->capacity * 31);
}

void scan_cache_init(ScanCache *sc) {
    memset(sc, 0, sizeof(ScanCache));
}

static void entry_free(ScanCacheEntry *e) {
    if (!e->valid || !e->docs) return;
    for (int i = 0; i < e->n_docs; i++)
        if (e->docs[i]) doc_free(e->docs[i]);
    free(e->docs);
    e->docs  = NULL;
    e->n_docs = 0;
    e->cap    = 0;
    e->valid  = 0;
}

void scan_cache_destroy(ScanCache *sc) {
    for (int i = 0; i < sc->n_entries; i++)
        entry_free(&sc->entries[i]);
    sc->n_entries = 0;
}

void scan_cache_invalidate(ScanCache *sc, const char *coll_name) {
    for (int i = 0; i < sc->n_entries; i++) {
        if (strcmp(sc->entries[i].coll_name, coll_name) == 0) {
            entry_free(&sc->entries[i]);
            return;
        }
    }
}

static ScanCacheEntry* find_or_alloc(ScanCache *sc, const char *name) {
    /* Find existing */
    for (int i = 0; i < sc->n_entries; i++)
        if (strcmp(sc->entries[i].coll_name, name) == 0)
            return &sc->entries[i];
    /* Alloc new slot */
    if (sc->n_entries >= SCAN_CACHE_MAX_COLLS) {
        /* Evict oldest valid entry (slot 0) — simple strategy */
        entry_free(&sc->entries[0]);
        memmove(&sc->entries[0], &sc->entries[1],
                (SCAN_CACHE_MAX_COLLS-1) * sizeof(ScanCacheEntry));
        sc->n_entries--;
    }
    ScanCacheEntry *e = &sc->entries[sc->n_entries++];
    memset(e, 0, sizeof(*e));
    strncpy(e->coll_name, name, sizeof(e->coll_name)-1);
    return e;
}

int scan_cache_get(ScanCache *sc, DiskDB *db, const char *coll_name,
                   Document ***docs_out)
{
    if (!sc || !db || !coll_name || !docs_out) return -1;

    DiskColl *coll = ddb_get_coll(db, coll_name);
    if (!coll) { *docs_out = NULL; return 0; }

    uint64_t fp = scan_cache_fingerprint(coll);
    ScanCacheEntry *e = find_or_alloc(sc, coll_name);

    /* Cache hit: valid + fingerprint unchanged */
    if (e->valid && e->fingerprint == fp) {
        *docs_out = e->docs;
        return e->n_docs;
    }

    /* Cache miss or stale: rebuild */
    entry_free(e);

    /* Use bulk scan: 1 read syscall for entire page range */
    Document **bulk_docs = NULL;
    int bulk_n = vec_bulk_scan(db, coll, &bulk_docs);
    if (bulk_n < 0 || !bulk_docs) { return 0; }

    e->docs  = bulk_docs; /* transfer ownership */
    e->cap   = bulk_n;
    e->n_docs = bulk_n;
    if (e->n_docs > SCAN_CACHE_MAX_DOCS) e->n_docs = SCAN_CACHE_MAX_DOCS;

    e->fingerprint = fp;
    e->valid       = 1;

    *docs_out = e->docs;
    return e->n_docs;
}
