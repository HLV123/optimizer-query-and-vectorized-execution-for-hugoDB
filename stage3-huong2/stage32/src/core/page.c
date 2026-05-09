/* page.c — Page manager implementation */
#include "page.h"
#include "serializer.h"
#include "checksum.h"

#include <string.h>
#include <stdlib.h>

/* ===== Page serialization =====
 * Fill buffer 4096 bytes từ HugoPage.
 * Checksum: tính CRC32 trên TOÀN buffer với 4 byte checksum field = 0,
 * sau đó ghi checksum vào đúng vị trí.
 */
void page_serialize(const HugoPage *p, uint8_t *buf) {
    memset(buf, 0, HUGO_PAGE_SIZE);
    write_u32_be(buf + PH_PAGEID_OFF,   p->page_id);
    buf[PH_TYPE_OFF] = p->page_type;
    write_u16_be(buf + PH_NUMKEYS_OFF,  p->num_keys);
    buf[PH_DIRTY_OFF] = 0;  /* dirty không persist */
    /* checksum để 0 trước, tính sau */
    write_u32_be(buf + PH_CHECKSUM_OFF, 0);
    /* reserved [12..18] đã = 0 do memset */
    memcpy(buf + HUGO_PAGE_HDR_SIZE, p->data, HUGO_PAGE_DATA_SIZE);

    uint32_t crc = hugo_crc32(buf, HUGO_PAGE_SIZE);
    write_u32_be(buf + PH_CHECKSUM_OFF, crc);
}

int page_deserialize(HugoPage *p, const uint8_t *buf) {
    /* Verify checksum: copy buf, zero checksum field, tính CRC, so sánh */
    uint32_t stored = read_u32_be(buf + PH_CHECKSUM_OFF);

    uint8_t tmp[HUGO_PAGE_SIZE];
    memcpy(tmp, buf, HUGO_PAGE_SIZE);
    write_u32_be(tmp + PH_CHECKSUM_OFF, 0);
    uint32_t computed = hugo_crc32(tmp, HUGO_PAGE_SIZE);
    if (stored != computed) return PG_ERR_CHECKSUM;

    p->page_id   = read_u32_be(buf + PH_PAGEID_OFF);
    p->page_type = buf[PH_TYPE_OFF];
    p->num_keys  = read_u16_be(buf + PH_NUMKEYS_OFF);
    p->dirty     = 0;
    p->checksum  = stored;
    memcpy(p->data, buf + HUGO_PAGE_HDR_SIZE, HUGO_PAGE_DATA_SIZE);
    return PG_OK;
}

/* ===== DB Header serialization (page 0) ===== */
static void serialize_header(const HugoHeader *h, uint8_t *buf) {
    memset(buf, 0, HUGO_PAGE_SIZE);
    write_u32_be(buf + DBH_MAGIC_OFF,     h->magic);
    write_u16_be(buf + DBH_VERSION_OFF,   h->version);
    write_u16_be(buf + DBH_PAGESIZE_OFF,  h->page_size);
    write_u64_be(buf + DBH_PAGECOUNT_OFF, h->page_count);
    write_u64_be(buf + DBH_ROOTPAGE_OFF,  h->root_page);
    write_u64_be(buf + DBH_FREELIST_OFF,  h->free_list);
    write_u32_be(buf + DBH_CHECKSUM_OFF,  0);
    /* reserved [36..67] = 0 do memset */
    uint32_t crc = hugo_crc32(buf, HUGO_PAGE_SIZE);
    write_u32_be(buf + DBH_CHECKSUM_OFF, crc);
}

static int deserialize_header(HugoHeader *h, const uint8_t *buf) {
    uint32_t stored = read_u32_be(buf + DBH_CHECKSUM_OFF);
    uint8_t tmp[HUGO_PAGE_SIZE];
    memcpy(tmp, buf, HUGO_PAGE_SIZE);
    write_u32_be(tmp + DBH_CHECKSUM_OFF, 0);
    uint32_t computed = hugo_crc32(tmp, HUGO_PAGE_SIZE);
    if (stored != computed) return PG_ERR_CHECKSUM;

    h->magic      = read_u32_be(buf + DBH_MAGIC_OFF);
    h->version    = read_u16_be(buf + DBH_VERSION_OFF);
    h->page_size  = read_u16_be(buf + DBH_PAGESIZE_OFF);
    h->page_count = read_u64_be(buf + DBH_PAGECOUNT_OFF);
    h->root_page  = read_u64_be(buf + DBH_ROOTPAGE_OFF);
    h->free_list  = read_u64_be(buf + DBH_FREELIST_OFF);
    h->checksum   = stored;

    if (h->magic != HUGO_MAGIC)        return PG_ERR_MAGIC;
    if (h->version != HUGO_VERSION)    return PG_ERR_VERSION;
    if (h->page_size != HUGO_PAGE_SIZE) return PG_ERR_VERSION;
    return PG_OK;
}

int pm_flush_header(PageManager *pm) {
    uint8_t buf[HUGO_PAGE_SIZE];
    serialize_header(&pm->hdr, buf);
    int rc = hugo_write(pm->file, buf, HUGO_PAGE_SIZE, 0);
    if (rc != HUGO_OK) return PG_ERR_IO;
    if (hugo_sync(pm->file) != HUGO_OK) return PG_ERR_IO;
    return PG_OK;
}

int pm_create(PageManager *pm, const char *path) {
    pm->file = hugo_open(path, HUGO_OPEN_RDWR | HUGO_OPEN_CREATE);
    if (!pm->file) return PG_ERR_IO;

    pm->hdr.magic      = HUGO_MAGIC;
    pm->hdr.version    = HUGO_VERSION;
    pm->hdr.page_size  = HUGO_PAGE_SIZE;
    pm->hdr.page_count = 1;     /* page 0 = header */
    pm->hdr.root_page  = 0;     /* chưa có B-tree */
    pm->hdr.free_list  = 0;     /* free list trống */

    return pm_flush_header(pm);
}

int pm_open(PageManager *pm, const char *path) {
    pm->file = hugo_open(path, HUGO_OPEN_RDWR);
    if (!pm->file) return PG_ERR_IO;

    uint8_t buf[HUGO_PAGE_SIZE];
    int rc = hugo_read(pm->file, buf, HUGO_PAGE_SIZE, 0);
    if (rc != HUGO_OK) { hugo_close(pm->file); pm->file = NULL; return PG_ERR_IO; }

    rc = deserialize_header(&pm->hdr, buf);
    if (rc != PG_OK) { hugo_close(pm->file); pm->file = NULL; return rc; }
    return PG_OK;
}

int pm_close(PageManager *pm) {
    if (!pm || !pm->file) return PG_OK;
    int rc = pm_flush_header(pm);
    hugo_close(pm->file);
    pm->file = NULL;
    return rc;
}

/* Allocate page mới ở cuối file (free list sẽ làm sau) */
int pm_alloc_page(PageManager *pm, uint64_t *out_page_id) {
    uint64_t pid = pm->hdr.page_count;
    pm->hdr.page_count++;
    *out_page_id = pid;
    /* Chưa flush header — caller có thể alloc nhiều page rồi flush 1 lần */
    return PG_OK;
}

int pm_read_page(PageManager *pm, uint64_t page_id, HugoPage *page) {
    if (page_id == 0 || page_id >= pm->hdr.page_count) return PG_ERR_RANGE;
    uint8_t buf[HUGO_PAGE_SIZE];
    int rc = hugo_read(pm->file, buf, HUGO_PAGE_SIZE,
                       page_id * HUGO_PAGE_SIZE);
    if (rc != HUGO_OK) return PG_ERR_IO;
    return page_deserialize(page, buf);
}

int pm_write_page(PageManager *pm, const HugoPage *page) {
    if (page->page_id == 0 || page->page_id >= pm->hdr.page_count)
        return PG_ERR_RANGE;
    uint8_t buf[HUGO_PAGE_SIZE];
    page_serialize(page, buf);
    int rc = hugo_write(pm->file, buf, HUGO_PAGE_SIZE,
                        page->page_id * HUGO_PAGE_SIZE);
    if (rc != HUGO_OK) return PG_ERR_IO;
    return PG_OK;
}
