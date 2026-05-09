/* doc_version.c — DocVersion serialization */
#include "doc_version.h"
#include "serializer.h"
#include <string.h>

int doc_version_serialize(const DocVersion *v, const uint8_t *data,
                          uint8_t *buf, size_t buf_size) {
    size_t needed = doc_version_total_size(v->data_size);
    if (buf_size < needed) return -1;

    uint8_t *p = buf;

    write_u64_be(p, v->version_id);       p += 8;
    write_u64_be(p, v->created_ts);       p += 8;
    write_u64_be(p, v->deleted_ts);       p += 8;
    write_u64_be(p, v->created_tx);       p += 8;
    write_u64_be(p, v->prev_version_ptr); p += 8;
    write_u32_be(p, v->data_size);        p += 4;

    if (v->data_size > 0 && data) {
        memcpy(p, data, v->data_size);
    }

    return (int)needed;
}

int doc_version_deserialize(const uint8_t *buf, size_t buf_size,
                             DocVersion *v_out, uint8_t *data_out, size_t data_out_size) {
    if (buf_size < DOC_VERSION_HDR_SIZE) return -1;

    const uint8_t *p = buf;

    v_out->version_id        = read_u64_be(p); p += 8;
    v_out->created_ts        = read_u64_be(p); p += 8;
    v_out->deleted_ts        = read_u64_be(p); p += 8;
    v_out->created_tx        = read_u64_be(p); p += 8;
    v_out->prev_version_ptr  = read_u64_be(p); p += 8;
    v_out->data_size         = read_u32_be(p); p += 4;
    v_out->data              = NULL;  /* caller xử lý */

    /* Validate: đủ bytes cho data không? */
    size_t total = (size_t)DOC_VERSION_HDR_SIZE + v_out->data_size;
    if (buf_size < total) return -1;

    if (v_out->data_size > 0 && data_out) {
        if (data_out_size < v_out->data_size) return -1;
        memcpy(data_out, p, v_out->data_size);
    }

    return (int)total;
}

int doc_version_peek_header(const uint8_t *buf, size_t buf_size, DocVersion *v_out) {
    if (buf_size < DOC_VERSION_HDR_SIZE) return -1;

    const uint8_t *p = buf;
    v_out->version_id        = read_u64_be(p); p += 8;
    v_out->created_ts        = read_u64_be(p); p += 8;
    v_out->deleted_ts        = read_u64_be(p); p += 8;
    v_out->created_tx        = read_u64_be(p); p += 8;
    v_out->prev_version_ptr  = read_u64_be(p); p += 8;
    v_out->data_size         = read_u32_be(p);
    v_out->data              = NULL;

    return 0;
}
