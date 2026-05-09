/* vec_scan_cache.h — In-memory document scan cache
 *
 * Vấn đề: ddb_read_doc() deserialise từng page từ disk mỗi query → chiếm
 * 97-98% thời gian. Với workload read-heavy (FUNDEN lặp lại), đây là
 * bottleneck tuyệt đối.
 *
 * Giải pháp: Lần đầu scan một collection, cache toàn bộ Document* vào
 * ScanCache. Các lần sau dùng trực tiếp cache — zero disk I/O.
 *
 * Invalidation: cache bị xóa khi collection bị modified (insert/update/delete).
 * DiskDB đã có field dirty để track; ta dùng c->count + c->next_id làm
 * fingerprint đơn giản.
 *
 * Thread safety: single-threaded, phù hợp với HugoDB MVP.
 *
 * Memory: owned Documents — freed khi cache evicted hoặc invalidated.
 */
#ifndef HUGO_VEC_SCAN_CACHE_H
#define HUGO_VEC_SCAN_CACHE_H

#include <stdint.h>
#include "../core/disk_db.h"
#include "../query/ast.h"
#include "../core/optimizer/arena.h"

#define SCAN_CACHE_MAX_COLLS  64       /* max collections cached */
#define SCAN_CACHE_MAX_DOCS   200000   /* max docs per collection */

typedef struct {
    char       coll_name[64];
    uint64_t   fingerprint;    /* c->count XOR c->next_id — invalidation key */
    Document **docs;           /* owned, heap-allocated */
    int        n_docs;
    int        cap;
    int        valid;
} ScanCacheEntry;

typedef struct {
    ScanCacheEntry entries[SCAN_CACHE_MAX_COLLS];
    int            n_entries;
} ScanCache;

/* Global cache — init once per process */
void scan_cache_init(ScanCache *sc);
void scan_cache_destroy(ScanCache *sc);

/* Get cached docs for collection. If cache miss or stale, reads from disk.
 * docs_out: pointer to Document** array (borrowed from cache — do NOT free)
 * Returns number of docs, or -1 on error.
 */
int scan_cache_get(ScanCache *sc, DiskDB *db, const char *coll_name,
                   Document ***docs_out);

/* Invalidate cache entry for a collection (call after write ops) */
void scan_cache_invalidate(ScanCache *sc, const char *coll_name);

/* Convenience: compute fingerprint from DiskColl */
uint64_t scan_cache_fingerprint(const DiskColl *c);

#endif
