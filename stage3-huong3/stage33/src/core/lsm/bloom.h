#ifndef LSM_BLOOM_H
#define LSM_BLOOM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct BloomFilter {
    uint32_t num_bits;
    uint8_t  num_hash_funcs;
    uint8_t *bits;
} BloomFilter;

BloomFilter *bloom_create(size_t num_keys, double fp_rate);
void         bloom_free(BloomFilter *b);
void         bloom_add(BloomFilter *b, const void *key, size_t kl);
bool         bloom_may_contain(const BloomFilter *b, const void *key, size_t kl);
/* Returns bytes written, 0 on error */
size_t       bloom_serialize(const BloomFilter *b, uint8_t *buf, size_t cap);
/* Returns NULL on error */
BloomFilter *bloom_deserialize(const uint8_t *buf, size_t size);

#endif
