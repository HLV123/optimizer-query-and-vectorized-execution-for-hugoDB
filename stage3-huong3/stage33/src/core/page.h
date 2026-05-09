/* page.h — Page manager
 *
 * Page format (4096 bytes total):
 *   [0..3]   page_id          uint32 BE
 *   [4]      page_type        uint8
 *   [5..6]   num_keys         uint16 BE
 *   [7]      dirty            uint8 (in-memory only, không tin khi đọc lên)
 *   [8..11]  checksum         uint32 BE  (CRC32 của page với 4 byte này = 0)
 *   [12..18] reserved (7 bytes)
 *   [19..4095] data (4077 bytes)
 *
 * Tổng header = 19 bytes. Data area = 4096 - 19 = 4077 bytes.
 *
 * DB Header (page 0):
 *   [0..3]   magic            0x48554755 ("HUGO")
 *   [4..5]   version          uint16 BE  = 1
 *   [6..7]   page_size        uint16 BE  = 4096
 *   [8..15]  page_count       uint64 BE
 *   [16..23] root_page        uint64 BE
 *   [24..31] free_list        uint64 BE
 *   [32..35] checksum         uint32 BE  (CRC32 với 4 byte này = 0)
 *   [36..67] reserved (32 bytes)
 *   phần còn lại đến 4096 = 0
 */
#ifndef HUGO_PAGE_H
#define HUGO_PAGE_H

#include <stdint.h>
#include "hugo_io.h"

#define HUGO_PAGE_SIZE       4096
#define HUGO_MAGIC           0x48554755u
#define HUGO_VERSION         1
#define HUGO_PAGE_HDR_SIZE   19
#define HUGO_PAGE_DATA_SIZE  (HUGO_PAGE_SIZE - HUGO_PAGE_HDR_SIZE)

/* Page types */
#define PAGE_TYPE_INTERNAL   0x01
#define PAGE_TYPE_LEAF       0x02
#define PAGE_TYPE_OVERFLOW   0x03
#define PAGE_TYPE_FREE       0x04
#define PAGE_TYPE_META       0x05

/* DB header offsets */
#define DBH_MAGIC_OFF        0
#define DBH_VERSION_OFF      4
#define DBH_PAGESIZE_OFF     6
#define DBH_PAGECOUNT_OFF    8
#define DBH_ROOTPAGE_OFF     16
#define DBH_FREELIST_OFF     24
#define DBH_CHECKSUM_OFF     32
#define DBH_HDR_SIZE         68  /* 36 + 32 reserved */

/* Page header offsets */
#define PH_PAGEID_OFF        0
#define PH_TYPE_OFF          4
#define PH_NUMKEYS_OFF       5
#define PH_DIRTY_OFF         7
#define PH_CHECKSUM_OFF      8

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t page_size;
    uint64_t page_count;
    uint64_t root_page;
    uint64_t free_list;
    uint32_t checksum;
} HugoHeader;

typedef struct {
    uint32_t page_id;
    uint8_t  page_type;
    uint16_t num_keys;
    uint8_t  dirty;
    uint32_t checksum;
    uint8_t  data[HUGO_PAGE_DATA_SIZE];
} HugoPage;

typedef struct {
    HugoFile  *file;
    HugoHeader hdr;
} PageManager;

/* Result codes */
#define PG_OK              0
#define PG_ERR_IO         -1
#define PG_ERR_MAGIC      -2
#define PG_ERR_VERSION    -3
#define PG_ERR_CHECKSUM   -4
#define PG_ERR_RANGE      -5
#define PG_ERR_NOMEM      -6

/* Lifecycle */
int  pm_create(PageManager *pm, const char *path);   /* tạo mới, init header */
int  pm_open  (PageManager *pm, const char *path);   /* mở file đã tồn tại */
int  pm_close (PageManager *pm);

/* Header */
int  pm_flush_header(PageManager *pm);

/* Page ops — page_id 0 reserved cho DB header */
int  pm_alloc_page(PageManager *pm, uint64_t *out_page_id);
int  pm_read_page (PageManager *pm, uint64_t page_id, HugoPage *page);
int  pm_write_page(PageManager *pm, const HugoPage *page);

/* Helpers — serialize/deserialize page <-> 4096-byte buffer */
void page_serialize  (const HugoPage *p, uint8_t *buf);   /* tự fill checksum */
int  page_deserialize(HugoPage *p, const uint8_t *buf);   /* verify checksum */

#endif
