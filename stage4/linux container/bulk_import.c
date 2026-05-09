/* bulk_import.c — Minimal JSONL parser, stream insert into DiskDB */
#include "bulk_import.h"
#include "collection.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

/* ===== Minimal JSON parser (single object per line) =====
 *
 * Scope: chỉ parse 1 JSON object flat. Giá trị hỗ trợ: string, number,
 * true/false/null. Nested object / array → SKIP (đọc qua).
 */

typedef struct {
    const char *p;
    const char *end;
    int error;
} JParser;

static void skip_ws(JParser *jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
                               *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static int expect(JParser *jp, char c) {
    skip_ws(jp);
    if (jp->p >= jp->end || *jp->p != c) { jp->error = 1; return -1; }
    jp->p++;
    return 0;
}

static int parse_string(JParser *jp, char *out, size_t out_cap) {
    skip_ws(jp);
    if (jp->p >= jp->end || *jp->p != '"') { jp->error = 1; return -1; }
    jp->p++;
    size_t len = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        char ch = *jp->p;
        if (ch == '\\' && jp->p + 1 < jp->end) {
            jp->p++;
            char esc = *jp->p;
            switch (esc) {
            case '"':  ch = '"';  break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/';  break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case 'u': {
                if (jp->p + 4 >= jp->end) { jp->error = 1; return -1; }
                unsigned int cp = 0;
                for (int i = 0; i < 4; i++) {
                    jp->p++;
                    char hc = *jp->p;
                    int d = (hc >= '0' && hc <= '9') ? hc - '0' :
                            (hc >= 'a' && hc <= 'f') ? hc - 'a' + 10 :
                            (hc >= 'A' && hc <= 'F') ? hc - 'A' + 10 : -1;
                    if (d < 0) { jp->error = 1; return -1; }
                    cp = (cp << 4) | d;
                }
                if (cp < 0x80) {
                    if (len + 1 < out_cap) out[len++] = (char)cp;
                } else if (cp < 0x800) {
                    if (len + 2 < out_cap) {
                        out[len++] = (char)(0xC0 | (cp >> 6));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    if (len + 3 < out_cap) {
                        out[len++] = (char)(0xE0 | (cp >> 12));
                        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                jp->p++;
                continue;
            }
            default: ch = esc;
            }
        }
        if (len + 1 < out_cap) out[len++] = ch;
        jp->p++;
    }
    if (jp->p >= jp->end) { jp->error = 1; return -1; }
    jp->p++;
    out[len] = 0;
    return 0;
}

static int parse_value(JParser *jp, Value *v);

static int skip_value(JParser *jp) {
    skip_ws(jp);
    if (jp->p >= jp->end) { jp->error = 1; return -1; }
    char c = *jp->p;
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        jp->p++;
        while (jp->p < jp->end && depth > 0) {
            char cc = *jp->p;
            if (cc == '"') {
                jp->p++;
                while (jp->p < jp->end && *jp->p != '"') {
                    if (*jp->p == '\\' && jp->p + 1 < jp->end) jp->p++;
                    jp->p++;
                }
                if (jp->p < jp->end) jp->p++;
                continue;
            }
            if (cc == '{' || cc == '[') depth++;
            else if (cc == '}' || cc == ']') {
                depth--;
                if (depth == 0 && cc == close) { jp->p++; return 0; }
            }
            jp->p++;
        }
        jp->error = 1;
        return -1;
    }
    Value dummy;
    return parse_value(jp, &dummy);
}

static int parse_value(JParser *jp, Value *v) {
    skip_ws(jp);
    if (jp->p >= jp->end) { jp->error = 1; return -1; }
    memset(v, 0, sizeof(*v));
    char c = *jp->p;

    if (c == '"') {
        v->type = VAL_STR;
        return parse_string(jp, v->str, sizeof(v->str));
    }
    if (c == 't' && jp->end - jp->p >= 4 && memcmp(jp->p, "true", 4) == 0) {
        v->type = VAL_BOOL; v->num = 1; jp->p += 4; return 0;
    }
    if (c == 'f' && jp->end - jp->p >= 5 && memcmp(jp->p, "false", 5) == 0) {
        v->type = VAL_BOOL; v->num = 0; jp->p += 5; return 0;
    }
    if (c == 'n' && jp->end - jp->p >= 4 && memcmp(jp->p, "null", 4) == 0) {
        v->type = VAL_NULL; jp->p += 4; return 0;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp;
        v->num = strtod(jp->p, &endp);
        if (endp == jp->p) { jp->error = 1; return -1; }
        v->type = VAL_NUM;
        jp->p = endp;
        return 0;
    }
    jp->error = 1;
    return -1;
}

static Document* parse_doc_line(const char *line, size_t len) {
    JParser jp;
    jp.p = line;
    jp.end = line + len;
    jp.error = 0;

    skip_ws(&jp);
    if (expect(&jp, '{') < 0) return NULL;

    Document *doc = (Document*)calloc(1, sizeof(Document));
    KVPair *tail = NULL;

    skip_ws(&jp);
    while (jp.p < jp.end && *jp.p != '}') {
        char key[128];
        if (parse_string(&jp, key, sizeof(key)) < 0) goto fail;
        skip_ws(&jp);
        if (expect(&jp, ':') < 0) goto fail;

        skip_ws(&jp);
        if (jp.p < jp.end && (*jp.p == '{' || *jp.p == '[')) {
            if (skip_value(&jp) < 0) goto fail;
        } else {
            Value v;
            if (parse_value(&jp, &v) < 0) goto fail;
            KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
            strncpy(kv->key, key, sizeof(kv->key)-1);
            kv->value = v;
            if (!doc->pairs) doc->pairs = tail = kv;
            else { tail->next = kv; tail = kv; }
            doc->count++;
        }

        skip_ws(&jp);
        if (jp.p < jp.end && *jp.p == ',') { jp.p++; skip_ws(&jp); }
    }
    if (expect(&jp, '}') < 0) goto fail;

    return doc;
fail:
    doc_free(doc);
    return NULL;
}

int bulk_import_jsonl(DiskDB *db, const char *coll_name,
                      FILE *in, BulkStats *stats) {
    memset(stats, 0, sizeof(*stats));
    if (!db || !coll_name || !in) return -1;

    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) c = ddb_create_coll(db, coll_name);
    if (!c) return -2;

    clock_t t0 = clock();

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char buf[65536];
    while (fgets(buf, sizeof(buf), in)) {
        stats->lines_read++;
        stats->bytes_read += strlen(buf);

        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r' ||
                            buf[blen-1] == ' '  || buf[blen-1] == '\t')) {
            buf[--blen] = 0;
        }
        if (blen == 0) continue;
        if (buf[0] == '#') continue;

        Document *d = parse_doc_line(buf, blen);
        if (!d) { stats->parse_errors++; continue; }

        uint64_t id;
        int rc = ddb_insert_doc(db, c, d, &id);
        doc_free(d);
        if (rc != 0) { stats->insert_errors++; continue; }
        stats->docs_inserted++;
    }
    free(line); (void)cap; (void)n;

    clock_t t1 = clock();
    stats->elapsed_sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
    return 0;
}

int bulk_import_file(DiskDB *db, const char *coll_name,
                     const char *path, BulkStats *stats) {
    FILE *f = fopen(path, "rb");
    if (!f) { memset(stats, 0, sizeof(*stats)); return -3; }
    int rc = bulk_import_jsonl(db, coll_name, f, stats);
    fclose(f);
    return rc;
}

int bulk_import_buffer(DiskDB *db, const char *coll_name,
                       const char *data, size_t len, BulkStats *stats) {
    memset(stats, 0, sizeof(*stats));
    if (!db || !coll_name || !data) return -1;

    DiskColl *c = ddb_get_coll(db, coll_name);
    if (!c) c = ddb_create_coll(db, coll_name);
    if (!c) return -2;

    clock_t t0 = clock();
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (line_len > 0 && (p[line_len-1] == '\r' ||
                                p[line_len-1] == ' ' ||
                                p[line_len-1] == '\t')) line_len--;

        stats->lines_read++;
        stats->bytes_read += line_len + (nl ? 1 : 0);

        if (line_len > 0 && p[0] != '#') {
            Document *d = parse_doc_line(p, line_len);
            if (!d) {
                stats->parse_errors++;
            } else {
                uint64_t id;
                int rc = ddb_insert_doc(db, c, d, &id);
                doc_free(d);
                if (rc != 0) stats->insert_errors++;
                else         stats->docs_inserted++;
            }
        }
        p = nl ? nl + 1 : end;
    }

    clock_t t1 = clock();
    stats->elapsed_sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
    return 0;
}
