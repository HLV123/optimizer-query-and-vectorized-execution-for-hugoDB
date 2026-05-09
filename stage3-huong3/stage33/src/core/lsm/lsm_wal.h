#ifndef LSM_WAL_H
#define LSM_WAL_H

#include <stdint.h>
#include <stddef.h>
#include "../hugo_io.h"
#include "memtable.h"

/* Record: [seq:8][op:1][key_len:2][val_len:4][key][val][crc32:4] */

typedef struct LsmWal {
    HugoFile *file;
    char      path[256];
    uint64_t  file_size;
    uint64_t  next_seq;
} LsmWal;

int lsm_wal_open  (LsmWal *w, const char *path);
int lsm_wal_append(LsmWal *w, uint8_t op,
                   const void *key, size_t kl,
                   const void *val, size_t vl,
                   uint64_t *out_seq);
int lsm_wal_sync  (LsmWal *w);
int lsm_wal_close (LsmWal *w);
int lsm_wal_delete(const char *path);

typedef int (*LsmWalReplayFn)(uint64_t seq, uint8_t op,
                               const void *key, size_t kl,
                               const void *val, size_t vl,
                               void *ctx);
int lsm_wal_replay(const char *path, LsmWalReplayFn fn, void *ctx);

#endif
