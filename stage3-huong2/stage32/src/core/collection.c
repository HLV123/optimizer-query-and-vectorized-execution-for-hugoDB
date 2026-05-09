/* collection.c — Document collection implementation */
#include "collection.h"
#include "serializer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== Document helpers ===== */
Document* doc_clone(const Document *src) {
    if (!src) return NULL;
    Document *d = (Document*)calloc(1, sizeof(Document));
    KVPair *tail = NULL;
    for (const KVPair *s = src->pairs; s; s = s->next) {
        KVPair *n = (KVPair*)calloc(1, sizeof(KVPair));
        strncpy(n->key, s->key, sizeof(n->key)-1);
        n->value = s->value;
        if (!d->pairs) d->pairs = tail = n;
        else { tail->next = n; tail = n; }
        d->count++;
    }
    return d;
}

void doc_free(Document *d) {
    if (!d) return;
    KVPair *kv = d->pairs;
    while (kv) { KVPair *n = kv->next; free(kv); kv = n; }
    free(d);
}

int doc_get_field(const Document *d, const char *field, Value *out) {
    if (!d) return -1;
    /* Try exact match first (handles dotted keys stored flat) */
    for (const KVPair *kv = d->pairs; kv; kv = kv->next) {
        if (strcmp(kv->key, field) == 0) {
            if (out) *out = kv->value;
            return 0;
        }
    }
    return -1;
}

void doc_set_field(Document *d, const char *field, Value v) {
    for (KVPair *kv = d->pairs; kv; kv = kv->next) {
        if (strcmp(kv->key, field) == 0) {
            kv->value = v;
            return;
        }
    }
    /* Not found — add */
    KVPair *n = (KVPair*)calloc(1, sizeof(KVPair));
    strncpy(n->key, field, sizeof(n->key)-1);
    n->value = v;
    KVPair **pp = &d->pairs;
    while (*pp) pp = &(*pp)->next;
    *pp = n;
    d->count++;
}

/* ===== Database lifecycle ===== */
int db_init(HugoDatabase *db, const char *name) {
    memset(db, 0, sizeof(*db));
    strncpy(db->name, name, sizeof(db->name)-1);
    return 0;
}

static void coll_free(Collection *c) {
    if (!c->docs) return;
    for (int i = 0; i < c->capacity; i++) {
        if (c->docs[i]) doc_free(c->docs[i]);
    }
    free(c->docs);
    c->docs = NULL;
    c->count = 0;
    c->capacity = 0;
}

void db_free(HugoDatabase *db) {
    for (int i = 0; i < db->n_collections; i++) {
        coll_free(&db->collections[i]);
    }
    db->n_collections = 0;
}

/* ===== Collection operations ===== */
Collection* db_get_collection(HugoDatabase *db, const char *name) {
    for (int i = 0; i < db->n_collections; i++) {
        if (strcmp(db->collections[i].name, name) == 0)
            return &db->collections[i];
    }
    return NULL;
}

Collection* db_create_collection(HugoDatabase *db, const char *name) {
    Collection *existing = db_get_collection(db, name);
    if (existing) return existing;
    if (db->n_collections >= MAX_COLLECTIONS) return NULL;
    Collection *c = &db->collections[db->n_collections++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name)-1);
    c->capacity = 64;
    c->docs = (Document**)calloc(c->capacity, sizeof(Document*));
    c->next_id = 1;
    db->dirty = 1;
    return c;
}

int db_drop_collection(HugoDatabase *db, const char *name) {
    for (int i = 0; i < db->n_collections; i++) {
        if (strcmp(db->collections[i].name, name) == 0) {
            coll_free(&db->collections[i]);
            /* Shift xuống */
            for (int j = i; j < db->n_collections - 1; j++)
                db->collections[j] = db->collections[j+1];
            db->n_collections--;
            db->dirty = 1;
            return 0;
        }
    }
    return -1;
}

int db_list_collections(const HugoDatabase *db, char names[][64], int max) {
    int n = (db->n_collections < max) ? db->n_collections : max;
    for (int i = 0; i < n; i++) {
        strncpy(names[i], db->collections[i].name, 64);
    }
    return db->n_collections;
}

/* ===== Document operations ===== */
static void coll_grow_if_needed(Collection *c) {
    if (c->next_id - 1 < (uint64_t)c->capacity) return;
    int new_cap = c->capacity * 2;
    Document **new_docs = (Document**)realloc(c->docs, sizeof(Document*) * new_cap);
    if (!new_docs) return;
    memset(new_docs + c->capacity, 0, sizeof(Document*) * (new_cap - c->capacity));
    c->docs = new_docs;
    c->capacity = new_cap;
}

uint64_t coll_insert(Collection *c, Document *doc) {
    coll_grow_if_needed(c);
    uint64_t id = c->next_id++;
    /* Auto-add "id" field nếu chưa có */
    Value check;
    if (doc_get_field(doc, "id", &check) != 0) {
        Value v; v.type = VAL_NUM; v.num = (double)id; v.str[0] = 0;
        doc_set_field(doc, "id", v);
    }
    c->docs[id - 1] = doc;
    c->count++;
    return id;
}

Document* coll_get(const Collection *c, uint64_t id) {
    if (id == 0 || id > (uint64_t)c->capacity) return NULL;
    return c->docs[id - 1];
}

int coll_delete(Collection *c, uint64_t id) {
    if (id == 0 || id > (uint64_t)c->capacity) return -1;
    if (!c->docs[id - 1]) return -1;
    doc_free(c->docs[id - 1]);
    c->docs[id - 1] = NULL;
    c->count--;
    return 0;
}

/* ===== Persistence (simple format — not using PageManager yet) =====
 * File format (all big-endian):
 *   magic         u32  = 0x48554755 (spec literal)
 *   version       u16  = 1
 *   n_collections u16
 *   for each collection:
 *     name_len   u16
 *     name       bytes
 *     next_id    u64
 *     n_docs     u32
 *     for each non-null doc:
 *       doc_id    u64
 *       n_pairs   u16
 *       for each pair:
 *         key_len  u16
 *         key      bytes
 *         val_type u8   (0=null, 1=num, 2=str, 3=bool)
 *         if num:  val_num    f64 (IEEE754 big-endian)
 *         if str:  str_len u16, str_bytes
 *         if bool: val_u8
 */

static void write_str(uint8_t **pp, const char *s) {
    uint16_t len = (uint16_t)strlen(s);
    write_u16_be(*pp, len); *pp += 2;
    memcpy(*pp, s, len); *pp += len;
}

static void write_value(uint8_t **pp, const Value *v) {
    **pp = (uint8_t)v->type; (*pp)++;
    if (v->type == VAL_NUM) {
        /* Reinterpret f64 as u64 for BE encoding */
        union { double d; uint64_t u; } u;
        u.d = v->num;
        write_u64_be(*pp, u.u); *pp += 8;
    } else if (v->type == VAL_STR) {
        write_str(pp, v->str);
    } else if (v->type == VAL_BOOL) {
        **pp = (uint8_t)(v->num != 0); (*pp)++;
    }
}

static int read_str(const uint8_t **pp, const uint8_t *end, char *out, size_t max) {
    if (*pp + 2 > end) return -1;
    uint16_t len = read_u16_be(*pp); *pp += 2;
    if (*pp + len > end) return -1;
    if (len >= max) len = (uint16_t)(max - 1);
    memcpy(out, *pp, len);
    out[len] = 0;
    *pp += len;
    return 0;
}

static int read_value(const uint8_t **pp, const uint8_t *end, Value *v) {
    memset(v, 0, sizeof(*v));
    if (*pp >= end) return -1;
    v->type = (ValType)**pp; (*pp)++;
    if (v->type == VAL_NUM) {
        if (*pp + 8 > end) return -1;
        union { double d; uint64_t u; } u;
        u.u = read_u64_be(*pp); *pp += 8;
        v->num = u.d;
    } else if (v->type == VAL_STR) {
        if (read_str(pp, end, v->str, sizeof(v->str)) < 0) return -1;
    } else if (v->type == VAL_BOOL) {
        if (*pp >= end) return -1;
        v->num = **pp ? 1 : 0; (*pp)++;
    }
    return 0;
}

int db_save(HugoDatabase *db, const char *path) {
    /* Compute size & alloc buffer */
    size_t cap = 64 * 1024;
    uint8_t *buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;
    uint8_t *p = buf;

    #define ENSURE(n) do { \
        size_t used = (size_t)(p - buf); \
        if (used + (n) > cap) { \
            cap = (used + (n)) * 2; \
            uint8_t *nb = (uint8_t*)realloc(buf, cap); \
            if (!nb) { free(buf); return -1; } \
            buf = nb; p = buf + used; \
        } \
    } while(0)

    ENSURE(8);
    write_u32_be(p, 0x48554755); p += 4;
    write_u16_be(p, 1); p += 2;
    write_u16_be(p, (uint16_t)db->n_collections); p += 2;

    for (int i = 0; i < db->n_collections; i++) {
        Collection *c = &db->collections[i];
        ENSURE(2 + strlen(c->name) + 8 + 4);
        write_str(&p, c->name);
        write_u64_be(p, c->next_id); p += 8;
        /* Count non-null docs */
        uint32_t n = 0;
        for (int j = 0; j < c->capacity; j++) if (c->docs[j]) n++;
        write_u32_be(p, n); p += 4;
        for (int j = 0; j < c->capacity; j++) {
            Document *d = c->docs[j];
            if (!d) continue;
            ENSURE(8 + 2);
            write_u64_be(p, (uint64_t)(j + 1)); p += 8;
            uint16_t n_pairs = (uint16_t)d->count;
            write_u16_be(p, n_pairs); p += 2;
            for (KVPair *kv = d->pairs; kv; kv = kv->next) {
                ENSURE(2 + strlen(kv->key) + 1 + 8 + 2 + sizeof(kv->value.str));
                write_str(&p, kv->key);
                write_value(&p, &kv->value);
            }
        }
    }
    #undef ENSURE

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)(p - buf), f);
    fclose(f);
    free(buf);
    db->dirty = 0;
    return 0;
}

int db_load(HugoDatabase *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 8) { fclose(f); return -1; }

    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    const uint8_t *p = buf;
    const uint8_t *end = buf + sz;

    if (end - p < 8) { free(buf); return -1; }
    uint32_t magic = read_u32_be(p); p += 4;
    if (magic != 0x48554755) { free(buf); return -1; }
    uint16_t version = read_u16_be(p); p += 2;
    if (version != 1) { free(buf); return -1; }
    uint16_t n_coll = read_u16_be(p); p += 2;

    db->n_collections = 0;
    for (uint16_t i = 0; i < n_coll && i < MAX_COLLECTIONS; i++) {
        char name[64];
        if (read_str(&p, end, name, sizeof(name)) < 0) goto fail;
        Collection *c = db_create_collection(db, name);
        if (!c) goto fail;
        if (end - p < 12) goto fail;
        c->next_id = read_u64_be(p); p += 8;
        uint32_t n_docs = read_u32_be(p); p += 4;
        /* Grow to fit */
        while ((int)(c->next_id + 1) > c->capacity) {
            int new_cap = c->capacity * 2;
            Document **nd = (Document**)realloc(c->docs, sizeof(Document*) * new_cap);
            memset(nd + c->capacity, 0, sizeof(Document*) * (new_cap - c->capacity));
            c->docs = nd;
            c->capacity = new_cap;
        }
        for (uint32_t j = 0; j < n_docs; j++) {
            if (end - p < 10) goto fail;
            uint64_t id = read_u64_be(p); p += 8;
            uint16_t n_pairs = read_u16_be(p); p += 2;
            Document *d = (Document*)calloc(1, sizeof(Document));
            KVPair *tail = NULL;
            for (uint16_t k = 0; k < n_pairs; k++) {
                KVPair *kv = (KVPair*)calloc(1, sizeof(KVPair));
                if (read_str(&p, end, kv->key, sizeof(kv->key)) < 0) goto fail;
                if (read_value(&p, end, &kv->value) < 0) goto fail;
                if (!d->pairs) d->pairs = tail = kv;
                else { tail->next = kv; tail = kv; }
                d->count++;
            }
            if (id > 0 && id <= (uint64_t)c->capacity) {
                c->docs[id - 1] = d;
                c->count++;
            } else {
                doc_free(d);
            }
        }
    }
    free(buf);
    db->dirty = 0;
    return 0;
fail:
    free(buf);
    return -1;
}
