#ifndef LSM_H
#define LSM_H

#include "memtable.h"
#include "lsm_wal.h"
#include "manifest.h"
#include "sstable.h"

#define LSM_L0_TRIGGER 4

typedef struct Lsm {
    char         dir_path[256];
    Memtable    *memtable;
    LsmWal       wal;
    char         wal_path[256];
    Memtable    *imm;
    char         imm_wal_path[256];
    LsmManifest  manifest;
    SSTableReader **readers;
    size_t         n_readers;
    size_t         readers_cap;
} Lsm;

Lsm *lsm_open (const char *dir);
int  lsm_close(Lsm *lsm);
int  lsm_put  (Lsm *lsm, const void *key, size_t kl, const void *val, size_t vl);
int  lsm_delete(Lsm *lsm, const void *key, size_t kl);
/* out_val caller must free */
int  lsm_get  (Lsm *lsm, const void *key, size_t kl, void **out_val, size_t *out_vl);

int  lsm_flush_memtable(Lsm *lsm);
int  lsm_compact_l0(Lsm *lsm);
int  lsm_compact_level(Lsm *lsm, int level);

typedef int (*LsmScanFn)(const void *key, size_t kl,
                          const void *val, size_t vl,
                          uint64_t seq, uint8_t op, void *ctx);
int lsm_scan(Lsm *lsm, LsmScanFn fn, void *ctx);

#endif
