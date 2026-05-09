/* doc_version.h — MVCC Document Version
 *
 * Mỗi document trong MVCC mode được lưu dưới dạng version chain:
 *   latest_version → prev_version → older_version → NULL
 *
 * DocVersion là append-only: khi update, tạo version MỚI trỏ về version cũ
 * thay vì sửa in-place. Khi delete, set deleted_ts trên version mới nhất.
 *
 * Version chain pointer: encode cả page_id + offset trong 1 uint64_t:
 *   bits [63..16] = page_id  (48 bits → tối đa 256TB với 4KB pages)
 *   bits [15..0]  = offset   (16 bits → tối đa 65535, fit trong 4077-byte data)
 *
 * Giá trị 0 nghĩa là "không có version trước" (NULL pointer).
 *
 * Trong MVCC mode, B-tree index value không còn là page_id raw nữa mà là
 * VERSION_PTR trỏ đến latest version của document đó.
 */
#ifndef HUGO_DOC_VERSION_H
#define HUGO_DOC_VERSION_H

#include <stdint.h>
#include <stddef.h>

/* ===== Version pointer encoding ===== */

/* Encode page_id + offset thành 1 uint64_t pointer */
static inline uint64_t version_ptr_encode(uint64_t page_id, uint16_t offset) {
    return (page_id << 16) | (uint64_t)offset;
}

/* Decode page_id từ version pointer */
static inline uint64_t version_ptr_page(uint64_t ptr) {
    return ptr >> 16;
}

/* Decode offset từ version pointer */
static inline uint16_t version_ptr_offset(uint64_t ptr) {
    return (uint16_t)(ptr & 0xFFFF);
}

#define VERSION_PTR_NULL  0   /* NULL pointer — không có version */

/* ===== DocVersion on-disk layout =====
 *
 * Serialized format (binary, big-endian):
 *   [0..7]    version_id        u64 BE
 *   [8..15]   created_ts        u64 BE  (commit_ts của tx tạo version, 0 nếu uncommitted)
 *   [16..23]  deleted_ts        u64 BE  (0 = alive; nonzero = deleted at this ts)
 *   [24..31]  created_tx        u64 BE  (tx_id của tx đã tạo version này)
 *   [32..39]  prev_version_ptr  u64 BE  (VERSION_PTR_NULL = không có version cũ hơn)
 *   [40..43]  data_size         u32 BE  (số bytes document data)
 *   [44..]    data              bytes   (serialized document)
 *
 * Total header = 44 bytes.
 */
#define DOC_VERSION_HDR_SIZE  44

typedef struct DocVersion {
    uint64_t  version_id;        /* unique, monotonic (dùng ts_oracle) */
    uint64_t  created_ts;        /* commit timestamp của tx tạo; 0 nếu đang uncommitted */
    uint64_t  deleted_ts;        /* 0 = còn sống; nonzero = bị delete tại ts này */
    uint64_t  created_tx;        /* tx_id để check visibility của uncommitted versions */
    uint64_t  prev_version_ptr;  /* version cũ hơn trong chain (VERSION_PTR_NULL = hết) */
    uint32_t  data_size;         /* số bytes tại data[] */
    uint8_t  *data;              /* pointer tới document bytes (NOT owned by this struct) */
} DocVersion;

/* ===== Serialization ===== */

/* Serialize DocVersion header + data vào buffer.
 * buf_size phải >= DOC_VERSION_HDR_SIZE + v->data_size.
 * Trả về số bytes đã ghi, hoặc -1 nếu buffer quá nhỏ. */
int doc_version_serialize(const DocVersion *v, const uint8_t *data,
                          uint8_t *buf, size_t buf_size);

/* Deserialize từ buffer vào DocVersion + data_out.
 * data_out phải đủ lớn (ít nhất data_size bytes, đọc từ header trước).
 * Trả về tổng bytes đã đọc, hoặc -1 nếu lỗi.
 * Sau khi return: v->data = NULL (caller set data_out vào nếu cần). */
int doc_version_deserialize(const uint8_t *buf, size_t buf_size,
                            DocVersion *v_out, uint8_t *data_out, size_t data_out_size);

/* Đọc chỉ header (44 bytes) để biết data_size trước khi alloc buffer.
 * Trả về 0 nếu OK, -1 nếu lỗi. */
int doc_version_peek_header(const uint8_t *buf, size_t buf_size, DocVersion *v_out);

/* Total size cần thiết để serialize version v với data_size bytes */
static inline size_t doc_version_total_size(uint32_t data_size) {
    return (size_t)DOC_VERSION_HDR_SIZE + data_size;
}

#endif
