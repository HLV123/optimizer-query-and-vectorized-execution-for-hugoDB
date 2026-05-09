/* bulk_import.h — Import JSONL into DiskDB collection
 *
 * Format: 1 JSON object / line, ví dụ:
 *   {"name":"Alice","age":20}
 *   {"name":"Bob","age":22,"city":"HN"}
 *
 * Parser tối giản: number, string (có escape \" \\ \n \r \t \uXXXX),
 * bool, null. Array + nested object — skip (không error, bỏ qua field đó).
 *
 * Return: số doc inserted thành công. Lỗi parse → skip dòng đó, tăng
 * stats.errors.
 */
#ifndef HUGO_BULK_IMPORT_H
#define HUGO_BULK_IMPORT_H

#include "disk_db.h"
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t  lines_read;
    uint64_t  docs_inserted;
    uint64_t  parse_errors;
    uint64_t  insert_errors;
    double    elapsed_sec;
    uint64_t  bytes_read;
} BulkStats;

/* Import từ FILE*, gọi cho flexibility (stdin, popen, ...)
 * coll_name: collection đích. Tự tạo nếu chưa có.
 * Returns 0 OK, <0 lỗi fatal. Per-line errors in stats. */
int bulk_import_jsonl(DiskDB *db, const char *coll_name,
                      FILE *in, BulkStats *stats);

/* Convenience: import từ path */
int bulk_import_file(DiskDB *db, const char *coll_name,
                     const char *path, BulkStats *stats);

/* Import từ buffer (dùng cho HTTP endpoint) */
int bulk_import_buffer(DiskDB *db, const char *coll_name,
                       const char *data, size_t len, BulkStats *stats);

#endif
