#ifndef LSM_SSTABLE_H
#define LSM_SSTABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "memtable.h"
#include "bloom.h"
#include "../hugo_io.h"

/* SSTable footer magic */
#define SST_MAGIC       0x55530000u
#define SST_FOOTER_SIZE 48
#define SST_BLOCK_SIZE  4096

/* Entry on-disk: [klen:2][vlen:4][seq:8][op:1][key][val][crc:4] */
#define SST_ENT_HDR  15
#define SST_ENT_CRC   4

/* ---- SSTableMetadata ---- */
typedef struct SSTableMetadata {
    uint64_t file_id;
    int      level;
    uint64_t file_size;
    uint64_t num_entries;
    uint64_t min_seq;
    uint64_t max_seq;
    char    *min_key;       /* heap-owned */
    size_t   min_key_len;
    char    *max_key;       /* heap-owned */
    size_t   max_key_len;
    char     path[256];
} SSTableMetadata;

void sstmeta_free(SSTableMetadata *m);   /* frees min_key/max_key only */
void sstmeta_destroy(SSTableMetadata *m); /* also frees m itself */

/* ---- Builder ---- */
typedef struct SSTableBuilder SSTableBuilder;
SSTableBuilder *sstable_builder_new(const char *path, size_t estimated_keys);
int             sstable_builder_add(SSTableBuilder *b, const MemtableEntry *e);
int             sstable_builder_finish(SSTableBuilder *b, SSTableMetadata *out);
void            sstable_builder_abandon(SSTableBuilder *b);

/* ---- Index entry (in-memory after parsing index block) ---- */
typedef struct {
    char    *key;          /* heap */
    size_t   key_len;
    uint64_t block_offset;
    uint32_t block_size;
} IdxEntry;

/* ---- Reader ---- */
typedef struct SSTableReader {
    HugoFile        *file;
    SSTableMetadata *meta;   /* borrowed */
    BloomFilter     *bloom;  /* owned */
    IdxEntry        *index;  /* owned array */
    size_t           index_len;
} SSTableReader;

SSTableReader *sstable_open(const char *path, SSTableMetadata *meta);
void           sstable_close(SSTableReader *r);
/* Returns LSM_OK, LSM_DELETED, LSM_NOT_FOUND. *out_val caller must free. */
int            sstable_get(SSTableReader *r,
                            const void *key, size_t kl,
                            void **out_val, size_t *out_vl,
                            uint64_t *out_seq);

/* ---- Iterator ---- */
typedef struct SSTableIterator {
    SSTableReader *reader;   /* borrowed */
    uint8_t       *blk;      /* current block buf (owned) */
    uint32_t       blk_size;
    uint32_t       blk_pos;
    int            blk_idx;  /* index into reader->index */
    MemtableEntry *cur;      /* current entry (owned) */
    bool           valid;
} SSTableIterator;

SSTableIterator    *sstable_iter_new(SSTableReader *r);
bool                sstable_iter_valid(const SSTableIterator *it);
int                 sstable_iter_next(SSTableIterator *it);
const MemtableEntry *sstable_iter_entry(const SSTableIterator *it);
void                sstable_iter_seek(SSTableIterator *it, const void *key, size_t kl);
void                sstable_iter_free(SSTableIterator *it);

#endif
