#include "sstable.h"
#include "../checksum.h"
#include <stdlib.h>
#include <string.h>

static uint16_t r16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t r32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static uint64_t r64(const uint8_t *p) { return ((uint64_t)r32(p)<<32)|r32(p+4); }

static int cmpk(const void *a, size_t al, const void *b, size_t bl) {
    size_t mn = al<bl?al:bl;
    int r = memcmp(a,b,mn); if(r) return r;
    if(al<bl) return -1; if(al>bl) return 1; return 0;
}

/* ---- Parse index block ---- */
static int parse_index(const uint8_t *buf, size_t sz, IdxEntry **out, size_t *out_len) {
    /* count entries */
    size_t count=0, pos=0;
    while (pos+2+12 <= sz) {
        uint16_t kl = r16(buf+pos);
        if (pos+2+kl+12 > sz) break;
        pos += 2+kl+12; count++;
    }
    if (!count) { *out=NULL; *out_len=0; return LSM_OK; }
    IdxEntry *arr = (IdxEntry *)calloc(count, sizeof(IdxEntry));
    if (!arr) return LSM_ERR_NOMEM;
    pos=0;
    for (size_t i=0; i<count; i++) {
        uint16_t kl = r16(buf+pos);
        arr[i].key = (char *)malloc(kl+1);
        if (!arr[i].key) {
            for (size_t j=0;j<i;j++) free(arr[j].key);
            free(arr); return LSM_ERR_NOMEM;
        }
        memcpy(arr[i].key, buf+pos+2, kl);
        arr[i].key[kl]='\0';
        arr[i].key_len      = kl;
        arr[i].block_offset = r64(buf+pos+2+kl);
        arr[i].block_size   = r32(buf+pos+2+kl+8);
        pos += 2+kl+12;
    }
    *out=arr; *out_len=count; return LSM_OK;
}

SSTableReader *sstable_open(const char *path, SSTableMetadata *meta) {
    if (!path) return NULL;
    HugoFile *f = hugo_open(path, HUGO_OPEN_RDONLY);
    if (!f) return NULL;
    int64_t fsz = hugo_size(f);
    if (fsz < SST_FOOTER_SIZE) { hugo_close(f); return NULL; }

    uint8_t footer[SST_FOOTER_SIZE];
    if (hugo_read(f, footer, SST_FOOTER_SIZE, (uint64_t)(fsz-SST_FOOTER_SIZE)) != HUGO_OK)
        { hugo_close(f); return NULL; }
    if (r32(footer+28) != SST_MAGIC) { hugo_close(f); return NULL; }

    uint64_t idx_off  = r64(footer+ 0);
    uint32_t idx_sz   = r32(footer+ 8);
    uint64_t blm_off  = r64(footer+12);
    uint32_t blm_sz   = r32(footer+20);

    /* bloom */
    BloomFilter *bloom = NULL;
    if (blm_sz >= 5) {
        uint8_t *bb = (uint8_t *)malloc(blm_sz);
        if (bb && hugo_read(f, bb, blm_sz, blm_off)==HUGO_OK)
            bloom = bloom_deserialize(bb, blm_sz);
        free(bb);
    }

    /* index */
    IdxEntry *index = NULL; size_t index_len = 0;
    if (idx_sz) {
        uint8_t *ib = (uint8_t *)malloc(idx_sz);
        if (ib && hugo_read(f, ib, idx_sz, idx_off)==HUGO_OK)
            parse_index(ib, idx_sz, &index, &index_len);
        free(ib);
    }

    SSTableReader *r = (SSTableReader *)calloc(1, sizeof(*r));
    if (!r) { hugo_close(f); bloom_free(bloom); return NULL; }
    r->file      = f;
    r->meta      = meta;
    r->bloom     = bloom;
    r->index     = index;
    r->index_len = index_len;
    return r;
}

void sstable_close(SSTableReader *r) {
    if (!r) return;
    hugo_close(r->file);
    bloom_free(r->bloom);
    if (r->index) {
        for (size_t i=0;i<r->index_len;i++) free(r->index[i].key);
        free(r->index);
    }
    free(r);
}

/* Scan one block; buf is a writable copy */
static int scan_block(uint8_t *buf, size_t sz,
                       const void *key, size_t kl,
                       void **out_val, size_t *out_vl, uint64_t *out_seq) {
    const MemtableEntry *best = NULL;
    size_t pos=0;
    while (pos + SST_ENT_HDR + SST_ENT_CRC <= sz) {
        uint16_t ek = r16(buf+pos);
        uint32_t ev = r32(buf+pos+2);
        uint64_t sq = r64(buf+pos+6);
        uint8_t  op = buf[pos+14];
        if (op!=LSM_OP_PUT && op!=LSM_OP_DELETE) break;
        size_t ent_sz = SST_ENT_HDR + ek + ev + SST_ENT_CRC;
        if (pos + ent_sz > sz) break;

        /* verify CRC */
        uint32_t saved = r32(buf+pos+SST_ENT_HDR+ek+ev);
        memset(buf+pos+SST_ENT_HDR+ek+ev, 0, SST_ENT_CRC);
        uint32_t calc  = hugo_crc32(buf+pos, ent_sz);
        buf[pos+SST_ENT_HDR+ek+ev+0] = (uint8_t)(saved>>24);
        buf[pos+SST_ENT_HDR+ek+ev+1] = (uint8_t)(saved>>16);
        buf[pos+SST_ENT_HDR+ek+ev+2] = (uint8_t)(saved>>8);
        buf[pos+SST_ENT_HDR+ek+ev+3] = (uint8_t)saved;
        if (calc != saved) { pos += ent_sz; continue; }

        if (cmpk(buf+pos+SST_ENT_HDR, ek, key, kl) == 0) {
            if (!best || sq > r64((const uint8_t *)best+6))
                best = (const MemtableEntry *)(buf+pos);
        }
        pos += ent_sz;
    }
    if (!best) return LSM_NOT_FOUND;
    uint16_t ek = r16((const uint8_t *)best);
    uint32_t ev = r32((const uint8_t *)best+2);
    uint8_t  op = ((const uint8_t *)best)[14];
    if (out_seq) *out_seq = r64((const uint8_t *)best+6);
    if (op == LSM_OP_DELETE) return LSM_DELETED;
    if (out_val && out_vl) {
        *out_vl = ev;
        *out_val = malloc(ev+1);
        if (!*out_val) return LSM_ERR_NOMEM;
        memcpy(*out_val, (const uint8_t *)best+SST_ENT_HDR+ek, ev);
        ((uint8_t *)*out_val)[ev]='\0';
    }
    return LSM_OK;
}

int sstable_get(SSTableReader *r, const void *key, size_t kl,
                void **out_val, size_t *out_vl, uint64_t *out_seq) {
    if (!r || !key || !kl) return LSM_NOT_FOUND;
    if (r->bloom && !bloom_may_contain(r->bloom, key, kl)) return LSM_NOT_FOUND;

    /* binary search index: find first entry where last_key >= key */
    if (!r->index_len) return LSM_NOT_FOUND;
    size_t lo=0, hi=r->index_len;
    while (lo<hi) {
        size_t mid = lo+(hi-lo)/2;
        if (cmpk(r->index[mid].key, r->index[mid].key_len, key, kl)<0) lo=mid+1;
        else hi=mid;
    }
    if (lo>=r->index_len) return LSM_NOT_FOUND;

    uint32_t bsz = r->index[lo].block_size;
    uint8_t *buf = (uint8_t *)malloc(bsz);
    if (!buf) return LSM_ERR_NOMEM;
    if (hugo_read(r->file, buf, bsz, r->index[lo].block_offset) != HUGO_OK) {
        free(buf); return LSM_ERR_IO;
    }
    int ret = scan_block(buf, bsz, key, kl, out_val, out_vl, out_seq);
    free(buf);
    return ret;
}

/* ---- Iterator ---- */
static int load_block(SSTableIterator *it, int idx) {
    free(it->blk); it->blk=NULL;
    free(it->cur); it->cur=NULL;
    it->valid=false;
    if (idx<0 || (size_t)idx>=it->reader->index_len) return LSM_NOT_FOUND;
    uint32_t bsz = it->reader->index[idx].block_size;
    it->blk = (uint8_t *)malloc(bsz);
    if (!it->blk) return LSM_ERR_NOMEM;
    if (hugo_read(it->reader->file, it->blk, bsz, it->reader->index[idx].block_offset)!=HUGO_OK)
        { free(it->blk); it->blk=NULL; return LSM_ERR_IO; }
    it->blk_size=bsz; it->blk_pos=0; it->blk_idx=idx;
    return LSM_OK;
}

static int advance(SSTableIterator *it) {
    free(it->cur); it->cur=NULL; it->valid=false;
    while (true) {
        if (it->blk && it->blk_pos + SST_ENT_HDR + SST_ENT_CRC <= it->blk_size) {
            uint8_t *p  = it->blk + it->blk_pos;
            uint16_t kl = r16(p);
            uint32_t vl = r32(p+2);
            uint8_t  op = p[14];
            if (op!=LSM_OP_PUT && op!=LSM_OP_DELETE) goto nextblk;
            size_t es = SST_ENT_HDR+kl+vl+SST_ENT_CRC;
            if (it->blk_pos+es > it->blk_size) goto nextblk;
            size_t tot = sizeof(MemtableEntry)-1+kl+vl;
            MemtableEntry *e = (MemtableEntry *)malloc(tot);
            if (!e) return LSM_ERR_NOMEM;
            e->seq_num   = r64(p+6);
            e->op_type   = op;
            e->key_len   = kl;
            e->value_len = vl;
            memcpy(e->data, p+SST_ENT_HDR, kl);
            if (vl) memcpy(e->data+kl, p+SST_ENT_HDR+kl, vl);
            it->blk_pos += (uint32_t)es;
            it->cur = e; it->valid=true;
            return LSM_OK;
        }
nextblk:
        if ((size_t)(it->blk_idx+1) >= it->reader->index_len) return LSM_NOT_FOUND;
        if (load_block(it, it->blk_idx+1) != LSM_OK) return LSM_ERR_IO;
    }
}

SSTableIterator *sstable_iter_new(SSTableReader *r) {
    if (!r) return NULL;
    SSTableIterator *it = (SSTableIterator *)calloc(1, sizeof(*it));
    if (!it) return NULL;
    it->reader  = r;
    it->blk_idx = -1;
    if (r->index_len) { load_block(it,0); advance(it); }
    return it;
}
bool sstable_iter_valid(const SSTableIterator *it) { return it && it->valid; }
int  sstable_iter_next(SSTableIterator *it) { return it ? advance(it) : LSM_NOT_FOUND; }
const MemtableEntry *sstable_iter_entry(const SSTableIterator *it) { return it ? it->cur : NULL; }

void sstable_iter_seek(SSTableIterator *it, const void *key, size_t kl) {
    if (!it || !key) return;
    SSTableReader *r = it->reader;
    size_t lo=0, hi=r->index_len;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (cmpk(r->index[mid].key, r->index[mid].key_len, key, kl)<0) lo=mid+1;
        else hi=mid;
    }
    load_block(it, (int)lo);
    /* advance to first entry >= key */
    while (advance(it)==LSM_OK && it->valid) {
        const MemtableEntry *e = it->cur;
        if (cmpk(entry_key(e), e->key_len, key, kl)>=0) break;
    }
}

void sstable_iter_free(SSTableIterator *it) {
    if (!it) return;
    free(it->blk); free(it->cur); free(it);
}
