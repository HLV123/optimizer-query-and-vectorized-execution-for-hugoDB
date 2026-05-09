#include "lsm_wal.h"
#include "../checksum.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Big-endian helpers */
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void w64(uint8_t *p, uint64_t v) { w32(p,(uint32_t)(v>>32)); w32(p+4,(uint32_t)v); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t r32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static uint64_t r64(const uint8_t *p) { return ((uint64_t)r32(p)<<32)|r32(p+4); }

/* Record fixed header size: seq(8)+op(1)+kl(2)+vl(4) = 15 */
#define WAL_FIXED  15
#define WAL_CRC     4

int lsm_wal_open(LsmWal *w, const char *path) {
    if (!w || !path) return LSM_ERR_IO;
    memset(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->file = hugo_open(path, HUGO_OPEN_RDWR | HUGO_OPEN_CREATE);
    if (!w->file) return LSM_ERR_IO;
    int64_t sz = hugo_size(w->file);
    w->file_size = sz > 0 ? (uint64_t)sz : 0;
    w->next_seq  = 1;
    return LSM_OK;
}

int lsm_wal_append(LsmWal *w, uint8_t op,
                   const void *key, size_t kl,
                   const void *val, size_t vl,
                   uint64_t *out_seq) {
    if (!w || !w->file || !key || kl == 0 || kl > 0xFFFF) return LSM_ERR_IO;
    if (op == LSM_OP_DELETE) { val = NULL; vl = 0; }
    if (vl > 0xFFFFFFFFUL) return LSM_ERR_IO;

    uint64_t seq = w->next_seq++;
    if (out_seq) *out_seq = seq;

    size_t rec_sz = WAL_FIXED + kl + vl + WAL_CRC;
    uint8_t *buf = (uint8_t *)malloc(rec_sz);
    if (!buf) return LSM_ERR_NOMEM;

    w64(buf+0, seq);
    buf[8] = op;
    w16(buf+9,  (uint16_t)kl);
    w32(buf+11, (uint32_t)vl);
    memcpy(buf + WAL_FIXED, key, kl);
    if (vl && val) memcpy(buf + WAL_FIXED + kl, val, vl);
    memset(buf + WAL_FIXED + kl + vl, 0, WAL_CRC);

    uint32_t crc = hugo_crc32(buf, rec_sz);
    w32(buf + WAL_FIXED + kl + vl, crc);

    int ret = hugo_write(w->file, buf, rec_sz, w->file_size);
    free(buf);
    if (ret != HUGO_OK) return LSM_ERR_IO;
    w->file_size += rec_sz;
    return LSM_OK;
}

int lsm_wal_sync(LsmWal *w) {
    if (!w || !w->file) return LSM_ERR_IO;
    return (hugo_sync(w->file) == HUGO_OK) ? LSM_OK : LSM_ERR_IO;
}

int lsm_wal_close(LsmWal *w) {
    if (!w) return LSM_OK;
    if (w->file) { hugo_close(w->file); w->file = NULL; }
    return LSM_OK;
}

int lsm_wal_delete(const char *path) {
    if (!path || !path[0]) return LSM_OK;
    remove(path);
    return LSM_OK;
}

int lsm_wal_replay(const char *path, LsmWalReplayFn fn, void *ctx) {
    if (!path || !fn) return LSM_OK;
    HugoFile *f = hugo_open(path, HUGO_OPEN_RDONLY);
    if (!f) return LSM_OK;  /* file doesn't exist yet — ok */
    int64_t fsz = hugo_size(f);
    if (fsz <= 0) { hugo_close(f); return LSM_OK; }

    uint64_t off = 0;
    uint8_t  hdr[WAL_FIXED];

    while (off + WAL_FIXED + WAL_CRC <= (uint64_t)fsz) {
        if (hugo_read(f, hdr, WAL_FIXED, off) != HUGO_OK) break;
        uint64_t seq = r64(hdr+0);
        uint8_t  op  = hdr[8];
        uint16_t kl  = r16(hdr+9);
        uint32_t vl  = r32(hdr+11);
        if (op != LSM_OP_PUT && op != LSM_OP_DELETE) break;
        if (op == LSM_OP_DELETE) vl = 0;

        size_t rec_sz = WAL_FIXED + kl + vl + WAL_CRC;
        if (off + rec_sz > (uint64_t)fsz) break;

        uint8_t *buf = (uint8_t *)malloc(rec_sz);
        if (!buf) break;
        if (hugo_read(f, buf, rec_sz, off) != HUGO_OK) { free(buf); break; }

        /* verify CRC */
        uint32_t stored = r32(buf + WAL_FIXED + kl + vl);
        memset(buf + WAL_FIXED + kl + vl, 0, WAL_CRC);
        uint32_t calc = hugo_crc32(buf, rec_sz);
        if (calc != stored) { free(buf); break; }

        fn(seq, op,
           buf + WAL_FIXED, kl,
           (vl ? buf + WAL_FIXED + kl : NULL), vl,
           ctx);
        off += rec_sz;
        free(buf);
    }
    hugo_close(f);
    return LSM_OK;
}
