/* vec_bulk_scan.c — Bulk page read implementation
 *
 * Cốt lõi: thay vì 30k lần pread(4096 bytes) riêng lẻ,
 * đọc toàn bộ range pages trong 1 lần rồi deserialize từ memory.
 *
 * Page layout trong file:
 *   offset = page_id * HUGO_PAGE_SIZE
 *   [0..3]   page_id     u32 BE
 *   [4]      page_type   u8
 *   [5..6]   num_keys    u16 BE
 *   [7]      dirty       u8
 *   [8..11]  checksum    u32 BE
 *   [12..4095] data      4084 bytes
 *
 * Doc data bắt đầu tại offset 12 trong mỗi page (= page.data[]).
 */
#include "vec_bulk_scan.h"
#include "../core/page.h"
#include "../core/hugo_io.h"
#include "../core/serializer.h"
#include "../core/collection.h"
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif

/* Page header offsets (từ page.c) */
#define PH_ID_OFF       0
#define PH_TYPE_OFF     4
#define PH_NUMKEYS_OFF  5
#define PH_DIRTY_OFF    7
#define PH_CRC_OFF      8   /* PH_CHECKSUM_OFF */
#define PH_DATA_OFF     19  /* = HUGO_PAGE_HDR_SIZE */
#define PAGE_TYPE_DOC   0x06

/* Deserialize doc từ raw page buffer (giống disk_db.c::deserialize_doc)
 * buf trỏ đến đầu page (4096 bytes).
 * Trả về Document* hoặc NULL. */
static Document* deser_doc_from_page(const uint8_t *page_buf)
{
    uint8_t ptype = page_buf[PH_TYPE_OFF];
    if (ptype != PAGE_TYPE_DOC) return NULL;

    const uint8_t *data = page_buf + PH_DATA_OFF;
    size_t data_size    = HUGO_PAGE_SIZE - PH_DATA_OFF;
    const uint8_t *p   = data;
    const uint8_t *end = data + data_size;

    if (end - p < 2) return NULL;
    uint16_t n_pairs = read_u16_be(p); p += 2;
    if (n_pairs == 0) return NULL; /* empty doc */

    Document *doc = (Document*)calloc(1, sizeof(Document));
    if (!doc) return NULL;

    KVPair *tail = NULL;
    for (uint16_t i = 0; i < n_pairs; i++) {
        if (end - p < 2) goto fail;
        uint16_t klen = read_u16_be(p); p += 2;
        if ((size_t)(end - p) < klen + 1) goto fail;

        KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
        if (!kv) goto fail;
        if (klen >= sizeof(kv->key)) klen = (uint16_t)(sizeof(kv->key) - 1);
        memcpy(kv->key, p, klen); kv->key[klen] = 0; p += klen;

        kv->value.type = (ValType)*p++;
        if (kv->value.type == VAL_NUM) {
            if (end - p < 8) { free(kv); goto fail; }
            union { double d; uint64_t u; } u;
            u.u = read_u64_be(p); p += 8;
            kv->value.num = u.d;
        } else if (kv->value.type == VAL_STR) {
            if (end - p < 2) { free(kv); goto fail; }
            uint16_t slen = read_u16_be(p); p += 2;
            if ((size_t)(end - p) < slen) { free(kv); goto fail; }
            if (slen >= sizeof(kv->value.str)) slen = (uint16_t)(sizeof(kv->value.str) - 1);
            memcpy(kv->value.str, p, slen); kv->value.str[slen] = 0;
            p += slen;
        } else if (kv->value.type == VAL_BOOL) {
            if (end - p < 1) { free(kv); goto fail; }
            kv->value.num = *p++ ? 1.0 : 0.0;
        }

        if (!doc->pairs) doc->pairs = tail = kv;
        else { tail->next = kv; tail = kv; }
        doc->count++;
    }
    return doc;

fail: {
    KVPair *kv = doc->pairs;
    while (kv) { KVPair *n = kv->next; free(kv); kv = n; }
    free(doc);
    return NULL;
    }
}

/* ===== vec_bulk_scan ===== */

int vec_bulk_scan(DiskDB *db, DiskColl *coll, Document ***docs_out)
{
    *docs_out = NULL;
    if (!db || !coll || coll->count == 0) return 0;

    /* ── Step 1: find page range ── */
    uint64_t pid_min = UINT64_MAX, pid_max = 0;
    int n_expected = 0;

    for (uint64_t id = 1; id < coll->capacity && id < coll->next_id; id++) {
        uint64_t pid = coll->doc_page_ids[id];
        if (pid == 0) continue;
        if (pid < pid_min) pid_min = pid;
        if (pid > pid_max) pid_max = pid;
        n_expected++;
    }
    if (n_expected == 0 || pid_min > pid_max) return 0;

    uint64_t n_pages   = pid_max - pid_min + 1;
    uint64_t read_size = n_pages * HUGO_PAGE_SIZE;

    /* Sanity check: không đọc hơn 512MB cùng lúc */
    if (read_size > 512ULL * 1024 * 1024) {
        /* Fallback: dùng ddb_read_doc bình thường */
        int cap = n_expected + 64;
        Document **arr = (Document**)malloc(cap * sizeof(Document*));
        if (!arr) return -1;
        int n = 0;
        for (uint64_t id = 1; id < coll->capacity && id < coll->next_id; id++) {
            if (coll->doc_page_ids[id] == 0) continue;
            Document *d = ddb_read_doc(db, coll, id);
            if (d) arr[n++] = d;
        }
        *docs_out = arr;
        return n;
    }

    /* ── Step 2: bulk read entire range ── */
    uint8_t *buf = (uint8_t*)malloc(read_size);
    if (!buf) return -1;

    uint64_t file_offset = pid_min * HUGO_PAGE_SIZE;
    int rc = hugo_read(db->pm.file, buf, (size_t)read_size, file_offset);
    if (rc != HUGO_OK) {
        free(buf);
        return -1;
    }

    /* ── Step 3: deserialize from memory buffer ── */
    int cap = n_expected + 16;
    Document **arr = (Document**)malloc(cap * sizeof(Document*));
    if (!arr) { free(buf); return -1; }

    int n = 0;
    for (uint64_t id = 1; id < coll->capacity && id < coll->next_id; id++) {
        uint64_t pid = coll->doc_page_ids[id];
        if (pid == 0) continue;
        if (pid < pid_min || pid > pid_max) continue; /* outside range */

        /* Prefetch next page into L2 cache while processing current */
        uint64_t next_pid = 0;
        for (uint64_t nid = id+1; nid < coll->capacity && nid < coll->next_id; nid++) {
            next_pid = coll->doc_page_ids[nid];
            if (next_pid != 0) break;
        }
        if (next_pid >= pid_min && next_pid <= pid_max)
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(buf + (next_pid - pid_min) * HUGO_PAGE_SIZE, 0, 1);
#elif defined(_MSC_VER)
            _mm_prefetch((const char*)(buf + (next_pid - pid_min) * HUGO_PAGE_SIZE), _MM_HINT_T1);
#endif

        uint64_t buf_offset = (pid - pid_min) * HUGO_PAGE_SIZE;
        Document *d = deser_doc_from_page(buf + buf_offset);
        if (!d) continue;

        if (n >= cap) {
            int new_cap = cap * 2;
            Document **na = (Document**)realloc(arr, new_cap * sizeof(Document*));
            if (!na) { doc_free(d); break; }
            arr = na; cap = new_cap;
        }
        arr[n++] = d;
    }

    free(buf);
    *docs_out = arr;
    return n;
}
