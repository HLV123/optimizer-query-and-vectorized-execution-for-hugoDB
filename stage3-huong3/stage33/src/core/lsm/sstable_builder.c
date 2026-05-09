#include "sstable.h"
#include "../checksum.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Big-endian helpers */
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void w64(uint8_t *p, uint64_t v) { w32(p,(uint32_t)(v>>32)); w32(p+4,(uint32_t)v); }

struct SSTableBuilder {
    HugoFile *file;
    char      path[256];
    uint64_t  write_offset;

    /* current data block buffer */
    uint8_t  *blk;
    uint32_t  blk_used;

    /* index buffer (grows) */
    uint8_t  *idx;
    size_t    idx_used, idx_cap;

    BloomFilter *bloom;
    size_t       estimated_keys;

    uint64_t num_entries;
    uint64_t min_seq, max_seq;
    char    *min_key; size_t min_key_len;
    char    *max_key; size_t max_key_len;

    /* last key in current block (for index) */
    char    *last_key; size_t last_key_len;
    uint64_t blk_start_offset;
    int      has_entry;
};

static int idx_append(struct SSTableBuilder *b,
                       const char *key, size_t kl,
                       uint64_t blk_off, uint32_t blk_sz) {
    /* [klen:2][key][block_offset:8][block_size:4] */
    size_t need = 2 + kl + 12;
    if (b->idx_used + need > b->idx_cap) {
        size_t nc = b->idx_cap ? b->idx_cap*2 : 64*1024;
        while (nc < b->idx_used + need) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(b->idx, nc);
        if (!nb) return LSM_ERR_NOMEM;
        b->idx = nb; b->idx_cap = nc;
    }
    uint8_t *p = b->idx + b->idx_used;
    w16(p, (uint16_t)kl);
    memcpy(p+2, key, kl);
    w64(p+2+kl,   blk_off);
    w32(p+2+kl+8, blk_sz);
    b->idx_used += need;
    return LSM_OK;
}

static int flush_block(struct SSTableBuilder *b) {
    if (!b->has_entry || b->blk_used == 0) return LSM_OK;
    if (hugo_write(b->file, b->blk, b->blk_used, b->write_offset) != HUGO_OK)
        return LSM_ERR_IO;
    int r = idx_append(b, b->last_key, b->last_key_len,
                       b->blk_start_offset, b->blk_used);
    if (r != LSM_OK) return r;
    b->write_offset    += b->blk_used;
    b->blk_start_offset = b->write_offset;
    b->blk_used         = 0;
    b->has_entry        = 0;
    return LSM_OK;
}

SSTableBuilder *sstable_builder_new(const char *path, size_t estimated_keys) {
    struct SSTableBuilder *b = (struct SSTableBuilder *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->file = hugo_open(path, HUGO_OPEN_RDWR | HUGO_OPEN_CREATE);
    if (!b->file) { free(b); return NULL; }
    b->blk = (uint8_t *)malloc(SST_BLOCK_SIZE * 4);
    if (!b->blk) { hugo_close(b->file); free(b); return NULL; }
    b->idx_cap = 64*1024;
    b->idx = (uint8_t *)malloc(b->idx_cap);
    if (!b->idx) { free(b->blk); hugo_close(b->file); free(b); return NULL; }
    b->estimated_keys = estimated_keys ? estimated_keys : 4096;
    b->min_seq = UINT64_MAX;
    return b;
}

int sstable_builder_add(struct SSTableBuilder *b, const MemtableEntry *e) {
    if (!b || !e) return LSM_ERR_IO;
    const uint8_t *key = entry_key(e);
    const uint8_t *val = entry_value(e);
    size_t kl = e->key_len;
    size_t vl = (e->op_type == LSM_OP_DELETE) ? 0 : e->value_len;
    size_t ent_sz = SST_ENT_HDR + kl + vl + SST_ENT_CRC;

    /* flush block if it would exceed block target */
    if (b->has_entry && b->blk_used + ent_sz > SST_BLOCK_SIZE) {
        int r = flush_block(b);
        if (r != LSM_OK) return r;
    }
    /* grow block buf if needed */
    if (b->blk_used + ent_sz > SST_BLOCK_SIZE * 4) {
        uint8_t *nb = (uint8_t *)realloc(b->blk, b->blk_used + ent_sz + SST_BLOCK_SIZE);
        if (!nb) return LSM_ERR_NOMEM;
        b->blk = nb;
    }

    uint8_t *p = b->blk + b->blk_used;
    w16(p+0,  (uint16_t)kl);
    w32(p+2,  (uint32_t)vl);
    w64(p+6,  e->seq_num);
    p[14] = e->op_type;
    memcpy(p + SST_ENT_HDR, key, kl);
    if (vl) memcpy(p + SST_ENT_HDR + kl, val, vl);
    memset(p + SST_ENT_HDR + kl + vl, 0, SST_ENT_CRC);
    uint32_t crc = hugo_crc32(p, ent_sz);
    w32(p + SST_ENT_HDR + kl + vl, crc);
    b->blk_used += (uint32_t)ent_sz;

    /* update last key in block */
    free(b->last_key);
    b->last_key = (char *)malloc(kl);
    if (!b->last_key) return LSM_ERR_NOMEM;
    memcpy(b->last_key, key, kl);
    b->last_key_len = kl;

    /* min/max key */
    if (!b->min_key) {
        b->min_key = (char *)malloc(kl);
        if (!b->min_key) return LSM_ERR_NOMEM;
        memcpy(b->min_key, key, kl);
        b->min_key_len = kl;
    }
    free(b->max_key);
    b->max_key = (char *)malloc(kl);
    if (!b->max_key) return LSM_ERR_NOMEM;
    memcpy(b->max_key, key, kl);
    b->max_key_len = kl;

    if (e->seq_num < b->min_seq) b->min_seq = e->seq_num;
    if (e->seq_num > b->max_seq) b->max_seq = e->seq_num;
    b->num_entries++;
    b->has_entry = 1;

    /* bloom */
    if (!b->bloom) b->bloom = bloom_create(b->estimated_keys, 0.01);
    if (b->bloom && e->op_type == LSM_OP_PUT) bloom_add(b->bloom, key, kl);

    return LSM_OK;
}

int sstable_builder_finish(struct SSTableBuilder *b, SSTableMetadata *out) {
    if (!b) return LSM_ERR_IO;

    int r = flush_block(b);
    if (r != LSM_OK) return r;

    /* bloom block */
    uint64_t bloom_off = b->write_offset;
    uint32_t bloom_sz  = 0;
    if (b->bloom) {
        size_t bytes = ((size_t)b->bloom->num_bits+7)/8;
        size_t bsz   = 5 + bytes;
        uint8_t *bb  = (uint8_t *)malloc(bsz);
        if (bb) {
            bloom_sz = (uint32_t)bloom_serialize(b->bloom, bb, bsz);
            hugo_write(b->file, bb, bloom_sz, b->write_offset);
            b->write_offset += bloom_sz;
            free(bb);
        }
    }

    /* index block */
    uint64_t idx_off = b->write_offset;
    uint32_t idx_sz  = (uint32_t)b->idx_used;
    if (idx_sz) {
        hugo_write(b->file, b->idx, idx_sz, b->write_offset);
        b->write_offset += idx_sz;
    }

    /* footer 48 bytes */
    uint8_t footer[SST_FOOTER_SIZE];
    memset(footer, 0, SST_FOOTER_SIZE);
    w64(footer+ 0, idx_off);
    w32(footer+ 8, idx_sz);
    w64(footer+12, bloom_off);
    w32(footer+20, bloom_sz);
    w32(footer+24, (uint32_t)b->num_entries);   /* num_entries in 4 bytes */
    w32(footer+28, SST_MAGIC);
    w64(footer+32, b->min_seq == UINT64_MAX ? 0 : b->min_seq);
    w64(footer+40, b->max_seq);

    hugo_write(b->file, footer, SST_FOOTER_SIZE, b->write_offset);
    b->write_offset += SST_FOOTER_SIZE;
    hugo_sync(b->file);
    hugo_close(b->file); b->file = NULL;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->file_size   = b->write_offset;
        out->num_entries = b->num_entries;
        out->min_seq     = b->min_seq == UINT64_MAX ? 0 : b->min_seq;
        out->max_seq     = b->max_seq;
        out->min_key     = b->min_key; b->min_key = NULL;
        out->min_key_len = b->min_key_len;
        out->max_key     = b->max_key; b->max_key = NULL;
        out->max_key_len = b->max_key_len;
        snprintf(out->path, sizeof(out->path), "%s", b->path);
    }

    free(b->blk); free(b->idx); free(b->last_key);
    free(b->min_key); free(b->max_key);
    bloom_free(b->bloom);
    free(b);
    return LSM_OK;
}

void sstable_builder_abandon(struct SSTableBuilder *b) {
    if (!b) return;
    if (b->file) hugo_close(b->file);
    free(b->blk); free(b->idx); free(b->last_key);
    free(b->min_key); free(b->max_key);
    bloom_free(b->bloom);
    free(b);
}

void sstmeta_free(SSTableMetadata *m) {
    if (!m) return;
    free(m->min_key); m->min_key = NULL;
    free(m->max_key); m->max_key = NULL;
}

void sstmeta_destroy(SSTableMetadata *m) {
    if (!m) return;
    sstmeta_free(m);
    free(m);
}
