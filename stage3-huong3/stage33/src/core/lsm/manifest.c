#include "manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static int atomic_rename(const char *src, const char *dst) {
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#else
static int atomic_rename(const char *src, const char *dst) {
    return rename(src, dst);
}
#endif

/* hex encode/decode for binary keys */
static void hex_enc(const char *k, size_t kl, char *out, size_t cap) {
    const char *h = "0123456789abcdef";
    size_t i;
    for (i = 0; i < kl && i*2+2 < cap; i++) {
        out[i*2]   = h[(unsigned char)k[i]>>4];
        out[i*2+1] = h[(unsigned char)k[i]&0xf];
    }
    out[i*2] = '\0';
}
static int hex_chr(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
static size_t hex_dec(const char *hex, char *out, size_t cap) {
    size_t i = 0;
    while (hex[0] && hex[1] && i < cap) {
        out[i++] = (char)((hex_chr(hex[0])<<4)|hex_chr(hex[1]));
        hex += 2;
    }
    return i;
}

static void mpath(const LsmManifest *m, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/manifest", m->dir_path);
}
static void tpath(const LsmManifest *m, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/manifest.tmp", m->dir_path);
}

int manifest_load(LsmManifest *m, const char *dir) {
    memset(m, 0, sizeof(*m));
    snprintf(m->dir_path, sizeof(m->dir_path), "%s", dir);
    for (int i = 0; i < LSM_MAX_LEVELS; i++) {
        m->level_cap[i] = 8;
        m->levels[i] = (SSTableMetadata **)calloc(8, sizeof(SSTableMetadata *));
        if (!m->levels[i]) return LSM_ERR_NOMEM;
    }
    m->next_file_id = 1;
    m->next_seq_num = 1;

    char path[300]; mpath(m, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return LSM_OK;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        size_t l = strlen(line);
        while (l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
        if (!l) continue;

        if (!strncmp(line,"NEXT_FILE_ID ",13))
            m->next_file_id = (uint64_t)strtoull(line+13,NULL,10);
        else if (!strncmp(line,"NEXT_SEQ ",9))
            m->next_seq_num = (uint64_t)strtoull(line+9,NULL,10);
        else if (!strncmp(line,"CURRENT_WAL ",12))
            m->current_wal_id = (uint64_t)strtoull(line+12,NULL,10);
        else if (!strncmp(line,"SSTABLE ",8)) {
            SSTableMetadata *meta = (SSTableMetadata *)calloc(1,sizeof(*meta));
            if (!meta) { fclose(f); return LSM_ERR_NOMEM; }
            char minhex[512]={0}, maxhex[512]={0};
            int lv=0;
            sscanf(line+8, "%llu %d %llu %llu %llu %llu %511s %511s %255s",
                   (unsigned long long*)&meta->file_id, &lv,
                   (unsigned long long*)&meta->file_size,
                   (unsigned long long*)&meta->num_entries,
                   (unsigned long long*)&meta->min_seq,
                   (unsigned long long*)&meta->max_seq,
                   minhex, maxhex, meta->path);
            meta->level = lv;
            meta->min_key = (char*)malloc(strlen(minhex)/2+1);
            meta->max_key = (char*)malloc(strlen(maxhex)/2+1);
            if (meta->min_key && meta->max_key) {
                meta->min_key_len = hex_dec(minhex, meta->min_key, strlen(minhex)/2+1);
                meta->max_key_len = hex_dec(maxhex, meta->max_key, strlen(maxhex)/2+1);
            }
            manifest_add_sstable(m, lv, meta);
        }
    }
    fclose(f);
    return LSM_OK;
}

int manifest_save(LsmManifest *m) {
    char tmp[300]; tpath(m, tmp, sizeof(tmp));
    FILE *f = fopen(tmp, "w");
    if (!f) return LSM_ERR_IO;
    fprintf(f,"NEXT_FILE_ID %llu\n",(unsigned long long)m->next_file_id);
    fprintf(f,"NEXT_SEQ %llu\n",    (unsigned long long)m->next_seq_num);
    fprintf(f,"CURRENT_WAL %llu\n", (unsigned long long)m->current_wal_id);
    for (int lv=0; lv<LSM_MAX_LEVELS; lv++) {
        for (size_t i=0; i<m->level_count[lv]; i++) {
            SSTableMetadata *meta = m->levels[lv][i];
            if (!meta) continue;
            char mh[512]={0}, xh[512]={0};
            if (meta->min_key) hex_enc(meta->min_key, meta->min_key_len, mh, sizeof(mh));
            if (meta->max_key) hex_enc(meta->max_key, meta->max_key_len, xh, sizeof(xh));
            if (!mh[0]) strcpy(mh,"00");
            if (!xh[0]) strcpy(xh,"00");
            fprintf(f,"SSTABLE %llu %d %llu %llu %llu %llu %s %s %s\n",
                    (unsigned long long)meta->file_id, meta->level,
                    (unsigned long long)meta->file_size,
                    (unsigned long long)meta->num_entries,
                    (unsigned long long)meta->min_seq,
                    (unsigned long long)meta->max_seq,
                    mh, xh, meta->path);
        }
    }
    fflush(f); fclose(f);
    char dst[300]; mpath(m, dst, sizeof(dst));
    return atomic_rename(tmp, dst)==0 ? LSM_OK : LSM_ERR_IO;
}

void manifest_destroy(LsmManifest *m) {
    if (!m) return;
    for (int i=0; i<LSM_MAX_LEVELS; i++) {
        for (size_t j=0; j<m->level_count[i]; j++)
            sstmeta_destroy(m->levels[i][j]);
        free(m->levels[i]);
    }
}

uint64_t manifest_next_file_id(LsmManifest *m) { return m->next_file_id++; }
uint64_t manifest_next_seq(LsmManifest *m)      { return m->next_seq_num++; }

int manifest_add_sstable(LsmManifest *m, int level, SSTableMetadata *meta) {
    if (level<0||level>=LSM_MAX_LEVELS) return LSM_ERR_IO;
    if (m->level_count[level] >= m->level_cap[level]) {
        size_t nc = m->level_cap[level]*2;
        SSTableMetadata **nb = (SSTableMetadata **)realloc(m->levels[level], nc*sizeof(*nb));
        if (!nb) return LSM_ERR_NOMEM;
        m->levels[level]=nb; m->level_cap[level]=nc;
    }
    meta->level = level;
    m->levels[level][m->level_count[level]++] = meta;
    return LSM_OK;
}

int manifest_remove_sstable(LsmManifest *m, int level, uint64_t file_id) {
    if (level<0||level>=LSM_MAX_LEVELS) return LSM_ERR_IO;
    for (size_t i=0; i<m->level_count[level]; i++) {
        if (m->levels[level][i] && m->levels[level][i]->file_id==file_id) {
            sstmeta_destroy(m->levels[level][i]);
            for (size_t j=i; j+1<m->level_count[level]; j++)
                m->levels[level][j]=m->levels[level][j+1];
            m->level_count[level]--;
            return LSM_OK;
        }
    }
    return LSM_NOT_FOUND;
}
