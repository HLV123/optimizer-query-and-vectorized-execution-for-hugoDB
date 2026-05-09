/* collection.h — Document collection (in-memory MVP)
 *
 * Spec: Document Database — Collections of Documents (JSON-like).
 *
 * MVP:
 *   - Document = linked list of KVPair (reuse AST KVPair)
 *   - Collection = array of Document*, auto-assign id
 *   - Database = array of Collections by name
 *   - Persist toàn database vào 1 file .hugo khi save()
 *
 * Future (Phase 8+): dùng dbtree làm primary index theo doc id,
 * extra indexes cho fields.
 */
#ifndef HUGO_COLLECTION_H
#define HUGO_COLLECTION_H

#include <stdint.h>
#include "../query/ast.h"

#define MAX_COLLECTIONS   64
#define MAX_DOCS_PER_COLL 100000

typedef struct {
    char       name[64];
    Document **docs;              /* docs[i] = Document* hoặc NULL (đã xóa) */
    int        capacity;
    int        count;             /* số document hiện có (không tính NULL) */
    uint64_t   next_id;           /* id tiếp theo */
} Collection;

typedef struct {
    char        name[64];
    Collection  collections[MAX_COLLECTIONS];
    int         n_collections;
    int         dirty;            /* cần save */
} HugoDatabase;

/* Lifecycle */
int  db_init   (HugoDatabase *db, const char *name);
void db_free   (HugoDatabase *db);
int  db_load   (HugoDatabase *db, const char *path);   /* load từ file */
int  db_save   (HugoDatabase *db, const char *path);   /* serialize ra file */

/* Collection operations */
Collection* db_get_collection   (HugoDatabase *db, const char *name);
Collection* db_create_collection(HugoDatabase *db, const char *name);
int         db_drop_collection  (HugoDatabase *db, const char *name);
int         db_list_collections (const HugoDatabase *db, char names[][64], int max);

/* Document operations */
uint64_t coll_insert(Collection *c, Document *doc);  /* returns id, owns doc */
Document* coll_get  (const Collection *c, uint64_t id);
int      coll_delete(Collection *c, uint64_t id);

/* Utility: deep clone document (cho owner transfer) */
Document* doc_clone(const Document *src);
void      doc_free (Document *d);

/* Get field value từ document (hỗ trợ dotted path "address.city") */
int doc_get_field(const Document *d, const char *field, Value *out);

/* Set field vào document (tạo nếu chưa có, override nếu có) */
void doc_set_field(Document *d, const char *field, Value v);

#endif
