/* hugo_api.c — DiskDB → JSON API */
#include "hugo_api.h"
#include "executor_disk.h"
#include "collection.h"
#include "../query/tokenizer.h"
#include "../query/parser.h"
#include "../query/executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== JSON builder (minimal, no deps) ===== */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} JsonBuf;

static void jb_init(JsonBuf *jb) {
    jb->cap = 512;
    jb->buf = (char*)malloc(jb->cap);
    jb->len = 0;
    jb->buf[0] = 0;
}

static void jb_grow(JsonBuf *jb, size_t need) {
    if (jb->len + need + 1 <= jb->cap) return;
    while (jb->len + need + 1 > jb->cap) jb->cap *= 2;
    jb->buf = (char*)realloc(jb->buf, jb->cap);
}

static void jb_append(JsonBuf *jb, const char *s) {
    size_t n = strlen(s);
    jb_grow(jb, n);
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = 0;
}

static void jb_append_escaped(JsonBuf *jb, const char *s) {
    /* Escape quotes, backslash, control chars */
    jb_grow(jb, strlen(s) * 6 + 2);
    jb->buf[jb->len++] = '"';
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            jb->buf[jb->len++] = '\\';
            jb->buf[jb->len++] = c;
        } else if (c == '\n') { jb->buf[jb->len++] = '\\'; jb->buf[jb->len++] = 'n'; }
        else if (c == '\r') { jb->buf[jb->len++] = '\\'; jb->buf[jb->len++] = 'r'; }
        else if (c == '\t') { jb->buf[jb->len++] = '\\'; jb->buf[jb->len++] = 't'; }
        else if (c < 0x20) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            memcpy(jb->buf + jb->len, tmp, 6);
            jb->len += 6;
        } else {
            jb->buf[jb->len++] = c;
        }
    }
    jb->buf[jb->len++] = '"';
    jb->buf[jb->len] = 0;
}

static void jb_append_int(JsonBuf *jb, long long n) {
    char tmp[32];
    int k = snprintf(tmp, sizeof(tmp), "%lld", n);
    jb_grow(jb, (size_t)k);
    memcpy(jb->buf + jb->len, tmp, k);
    jb->len += k;
    jb->buf[jb->len] = 0;
}

static void jb_append_double(JsonBuf *jb, double d) {
    char tmp[64];
    int k;
    if (d == (double)(long long)d)
        k = snprintf(tmp, sizeof(tmp), "%lld", (long long)d);
    else
        k = snprintf(tmp, sizeof(tmp), "%g", d);
    jb_grow(jb, (size_t)k);
    memcpy(jb->buf + jb->len, tmp, k);
    jb->len += k;
    jb->buf[jb->len] = 0;
}

/* Serialize 1 Value → JSON */
static void jb_append_value(JsonBuf *jb, const Value *v) {
    switch (v->type) {
    case VAL_NUM:  jb_append_double(jb, v->num); break;
    case VAL_STR:  jb_append_escaped(jb, v->str); break;
    case VAL_BOOL: jb_append(jb, v->num != 0 ? "true" : "false"); break;
    case VAL_NULL: default: jb_append(jb, "null");
    }
}

/* Serialize Document → JSON object */
static void jb_append_document(JsonBuf *jb, const Document *d) {
    jb_append(jb, "{");
    int first = 1;
    for (const KVPair *kv = d->pairs; kv; kv = kv->next) {
        if (!first) jb_append(jb, ",");
        jb_append_escaped(jb, kv->key);
        jb_append(jb, ":");
        jb_append_value(jb, &kv->value);
        first = 0;
    }
    jb_append(jb, "}");
}

/* ===== API: exec HugoQL ===== */
char* hugo_api_exec(DiskDB *db, const char *hugoql) {
    JsonBuf jb; jb_init(&jb);

    /* Parse */
    TokenList tl;
    if (hugo_tokenize(hugoql, &tl) != 0) {
        jb_append(&jb, "{\"ok\":false,\"err_code\":\"TOKEN\",\"err_msg\":");
        jb_append_escaped(&jb, tl.error_msg);
        jb_append(&jb, "}");
        return jb.buf;
    }
    Query q;
    if (hugo_parse(&tl, &q) != 0) {
        jb_append(&jb, "{\"ok\":false,\"err_code\":\"PARSE\",\"err_msg\":");
        jb_append_escaped(&jb, q.error);
        jb_append(&jb, "}");
        query_free(&q);
        return jb.buf;
    }

    /* Execute */
    HugoResult r;
    hugo_execute_disk(db, &q, &r);

    if (!r.ok) {
        jb_append(&jb, "{\"ok\":false,\"err_code\":");
        jb_append_escaped(&jb, r.err_code);
        jb_append(&jb, ",\"err_msg\":");
        jb_append_escaped(&jb, r.err_msg);
        jb_append(&jb, "}");
    } else {
        jb_append(&jb, "{\"ok\":true,\"count\":");
        jb_append_int(&jb, r.count);
        if (r.info[0]) {
            jb_append(&jb, ",\"info\":");
            jb_append_escaped(&jb, r.info);
        }
        jb_append(&jb, ",\"docs\":[");
        int first = 1;
        for (int i = 0; i < r.count; i++) {
            if (!r.docs[i]) continue;
            if (!first) jb_append(&jb, ",");
            jb_append_document(&jb, r.docs[i]);
            first = 0;
        }
        jb_append(&jb, "]}");
    }

    result_free_disk(&r);
    query_free(&q);
    return jb.buf;
}

/* ===== API: list collections ===== */
char* hugo_api_list_collections(DiskDB *db) {
    JsonBuf jb; jb_init(&jb);
    jb_append(&jb, "{\"ok\":true,\"collections\":[");
    for (int i = 0; i < db->n_colls; i++) {
        if (i > 0) jb_append(&jb, ",");
        jb_append(&jb, "{\"name\":");
        jb_append_escaped(&jb, db->colls[i].name);
        jb_append(&jb, ",\"count\":");
        jb_append_int(&jb, (long long)db->colls[i].count);
        jb_append(&jb, ",\"next_id\":");
        jb_append_int(&jb, (long long)db->colls[i].next_id);
        jb_append(&jb, "}");
    }
    jb_append(&jb, "]}");
    return jb.buf;
}

/* ===== API: health ===== */
char* hugo_api_health(DiskDB *db) {
    JsonBuf jb; jb_init(&jb);
    jb_append(&jb, "{\"status\":\"ok\",\"db\":");
    jb_append_escaped(&jb, db->name);
    jb_append(&jb, ",\"collections\":");
    jb_append_int(&jb, db->n_colls);
    jb_append(&jb, ",\"wal_enabled\":");
    jb_append(&jb, db->wal_enabled ? "true" : "false");
    jb_append(&jb, "}");
    return jb.buf;
}

/* ===== MVCC Vacuum API ===== */
#include "mvcc_vacuum.h"

char* hugo_api_vacuum(DiskDB *db) {
    if (db->mode != HUGO_MODE_MVCC) {
        return strdup("{\"ok\":false,\"err\":\"vacuum only supported in MVCC mode\"}");
    }
    VacuumStats stats;
    int rc = mvcc_vacuum(db, &stats);
    /* Build JSON response */
    char buf[256];
    if (rc == 0) {
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"versions_removed\":%llu,\"pages_freed\":%llu,"
                 "\"oldest_visible_ts\":%llu}",
                 (unsigned long long)stats.versions_removed,
                 (unsigned long long)stats.pages_freed,
                 (unsigned long long)stats.oldest_visible_ts);
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"vacuum failed\"}");
    }
    return strdup(buf);
}
