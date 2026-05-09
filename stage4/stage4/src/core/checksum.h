/* checksum.h — CRC32 (IEEE 802.3 polynomial 0xEDB88320) */
#ifndef HUGO_CHECKSUM_H
#define HUGO_CHECKSUM_H

#include <stdint.h>
#include <stddef.h>

uint32_t hugo_crc32(const uint8_t *data, size_t len);

#endif
