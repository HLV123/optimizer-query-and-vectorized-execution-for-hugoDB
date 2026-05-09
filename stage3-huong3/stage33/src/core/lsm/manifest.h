#ifndef LSM_MANIFEST_H
#define LSM_MANIFEST_H

#include <stdint.h>
#include "sstable.h"

#define LSM_MAX_LEVELS 7

typedef struct LsmManifest {
    char dir_path[256];
    SSTableMetadata **levels[LSM_MAX_LEVELS];
    size_t            level_count[LSM_MAX_LEVELS];
    size_t            level_cap[LSM_MAX_LEVELS];
    uint64_t next_file_id;
    uint64_t next_seq_num;
    uint64_t current_wal_id;
} LsmManifest;

int      manifest_load(LsmManifest *m, const char *dir);
int      manifest_save(LsmManifest *m);
void     manifest_destroy(LsmManifest *m);
uint64_t manifest_next_file_id(LsmManifest *m);
uint64_t manifest_next_seq(LsmManifest *m);
int      manifest_add_sstable(LsmManifest *m, int level, SSTableMetadata *meta);
int      manifest_remove_sstable(LsmManifest *m, int level, uint64_t file_id);

#endif
