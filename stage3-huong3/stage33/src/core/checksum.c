/* checksum.c — CRC32 table-based implementation */
#include "checksum.h"

static uint32_t crc_table[256];
static int crc_table_init = 0;

static void init_crc_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_table_init = 1;
}

uint32_t hugo_crc32(const uint8_t *data, size_t len) {
    if (!crc_table_init) init_crc_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}
