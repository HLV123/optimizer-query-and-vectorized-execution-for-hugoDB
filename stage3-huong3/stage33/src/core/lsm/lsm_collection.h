#ifndef LSM_COLLECTION_H
#define LSM_COLLECTION_H

#include <stdint.h>
#include "lsm.h"
#include "../../query/ast.h"

typedef enum { HUGO_ENGINE_BTREE=0, HUGO_ENGINE_LSM=1 } HugoStorageEngine;

typedef struct {
    char      name[64];
    Lsm      *lsm;
    char      dir_path[256];
    uint64_t  next_id;
} LsmCollection;

LsmCollection *lsm_coll_open (const char *base_dir, const char *name);
int            lsm_coll_close(LsmCollection *lc);
int            lsm_coll_insert(LsmCollection *lc, Document *doc, uint64_t *out_id);
Document      *lsm_coll_get  (LsmCollection *lc, uint64_t id);
int            lsm_coll_update(LsmCollection *lc, uint64_t id, Document *doc);
int            lsm_coll_delete(LsmCollection *lc, uint64_t id);
typedef void (*LsmCollScanFn)(uint64_t id, Document *doc, void *ctx);
int            lsm_coll_scan(LsmCollection *lc, LsmCollScanFn fn, void *ctx);

void     id_to_key(uint64_t id, uint8_t key[8]);
uint64_t key_to_id(const uint8_t key[8]);
int      doc_serialize  (const Document *doc, uint8_t **out, size_t *out_len);
Document *doc_deserialize(const uint8_t *buf, size_t len);

#endif
