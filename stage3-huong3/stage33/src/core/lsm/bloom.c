#include "bloom.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t fnv1a(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void set_bit(uint8_t *bits, uint32_t idx)          { bits[idx >> 3] |= (uint8_t)(1u << (idx & 7)); }
static bool get_bit(const uint8_t *bits, uint32_t idx)    { return !!(bits[idx >> 3] & (1u << (idx & 7))); }

BloomFilter *bloom_create(size_t num_keys, double fp_rate) {
    if (!num_keys)  num_keys = 1;
    if (fp_rate <= 0) fp_rate = 0.01;
    if (fp_rate >= 1) fp_rate = 0.5;

    /* m = -n*ln(p) / (ln2)^2 */
    double ln2  = 0.693147180559945;
    double m_f  = -(double)num_keys * log(fp_rate) / (ln2 * ln2);
    uint32_t nb = (uint32_t)(m_f + 0.5);
    if (nb < 8) nb = 8;

    uint8_t k = (uint8_t)(((double)nb / (double)num_keys) * ln2 + 0.5);
    if (k < 1)  k = 1;
    if (k > 30) k = 30;

    BloomFilter *b = (BloomFilter *)malloc(sizeof(BloomFilter));
    if (!b) return NULL;
    size_t byte_count = ((size_t)nb + 7) / 8;
    b->bits = (uint8_t *)calloc(byte_count, 1);
    if (!b->bits) { free(b); return NULL; }
    b->num_bits       = nb;
    b->num_hash_funcs = k;
    return b;
}

void bloom_free(BloomFilter *b) {
    if (!b) return;
    free(b->bits);
    free(b);
}

void bloom_add(BloomFilter *b, const void *key, size_t kl) {
    if (!b || !key) return;
    uint64_t h1 = fnv1a(key, kl, 0);
    uint64_t h2 = fnv1a(key, kl, h1);
    for (uint8_t i = 0; i < b->num_hash_funcs; i++) {
        uint32_t idx = (uint32_t)((h1 + (uint64_t)i * h2) % b->num_bits);
        set_bit(b->bits, idx);
    }
}

bool bloom_may_contain(const BloomFilter *b, const void *key, size_t kl) {
    if (!b || !key) return true;
    uint64_t h1 = fnv1a(key, kl, 0);
    uint64_t h2 = fnv1a(key, kl, h1);
    for (uint8_t i = 0; i < b->num_hash_funcs; i++) {
        uint32_t idx = (uint32_t)((h1 + (uint64_t)i * h2) % b->num_bits);
        if (!get_bit(b->bits, idx)) return false;
    }
    return true;
}

/* Format: [num_bits:4 BE][num_hash_funcs:1][bits...] */
size_t bloom_serialize(const BloomFilter *b, uint8_t *buf, size_t cap) {
    if (!b || !buf) return 0;
    size_t bytes = ((size_t)b->num_bits + 7) / 8;
    size_t total = 5 + bytes;
    if (cap < total) return 0;
    buf[0] = (uint8_t)(b->num_bits >> 24);
    buf[1] = (uint8_t)(b->num_bits >> 16);
    buf[2] = (uint8_t)(b->num_bits >>  8);
    buf[3] = (uint8_t)(b->num_bits);
    buf[4] = b->num_hash_funcs;
    memcpy(buf + 5, b->bits, bytes);
    return total;
}

BloomFilter *bloom_deserialize(const uint8_t *buf, size_t size) {
    if (!buf || size < 5) return NULL;
    uint32_t nb = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                  ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    uint8_t  k  = buf[4];
    size_t bytes = ((size_t)nb + 7) / 8;
    if (size < 5 + bytes) return NULL;
    BloomFilter *b = (BloomFilter *)malloc(sizeof(BloomFilter));
    if (!b) return NULL;
    b->bits = (uint8_t *)malloc(bytes);
    if (!b->bits) { free(b); return NULL; }
    b->num_bits       = nb;
    b->num_hash_funcs = k;
    memcpy(b->bits, buf + 5, bytes);
    return b;
}
