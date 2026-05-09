#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/core/lsm/lsm_collection.h"

#define BASE "/tmp/lsm_p9"

static void rmrf(const char *d){ char cmd[300]; snprintf(cmd,sizeof(cmd),"rm -rf %s",d); system(cmd); }

static Document *mkdoc(const char *k, const char *val){
    Document *d=(Document*)calloc(1,sizeof(*d));
    KVPair *kv=(KVPair*)calloc(1,sizeof(*kv));
    snprintf(kv->key,sizeof(kv->key),"%s",k);
    kv->value.type=VAL_STR;
    snprintf(kv->value.str,sizeof(kv->value.str),"%s",val);
    d->pairs=kv; d->count=1; return d;
}
static void freedoc(Document *d){
    KVPair *p=d?d->pairs:NULL;
    while(p){KVPair *n=p->next;free(p);p=n;}
    free(d);
}

int main(void){
    printf("=== Phase 9: LsmCollection ===\n");
    rmrf(BASE);

    /* basic CRUD */
    LsmCollection *lc=lsm_coll_open(BASE,"users"); assert(lc);
    Document *doc=mkdoc("name","alice");
    uint64_t id;
    assert(lsm_coll_insert(lc,doc,&id)==LSM_OK);
    freedoc(doc);
    Document *got=lsm_coll_get(lc,id);
    assert(got && strcmp(got->pairs->value.str,"alice")==0);
    freedoc(got);
    Document *upd=mkdoc("name","bob");
    lsm_coll_update(lc,id,upd); freedoc(upd);
    got=lsm_coll_get(lc,id);
    assert(got && strcmp(got->pairs->value.str,"bob")==0); freedoc(got);
    lsm_coll_delete(lc,id);
    assert(lsm_coll_get(lc,id)==NULL);
    lsm_coll_close(lc);
    printf("  PASS: basic CRUD\n");

    /* persist */
    lc=lsm_coll_open(BASE,"logs"); assert(lc);
    doc=mkdoc("event","login");
    lsm_coll_insert(lc,doc,&id); freedoc(doc);
    lsm_coll_close(lc);
    lc=lsm_coll_open(BASE,"logs"); assert(lc);
    got=lsm_coll_get(lc,id);
    assert(got && strcmp(got->pairs->value.str,"login")==0); freedoc(got);
    lsm_coll_close(lc);
    printf("  PASS: persist across restart\n");

    /* 100 inserts */
    lc=lsm_coll_open(BASE,"batch"); assert(lc);
    uint64_t ids[100];
    for(int i=0;i<100;i++){
        char v[20]; snprintf(v,20,"item-%d",i);
        Document *d=mkdoc("v",v); lsm_coll_insert(lc,d,&ids[i]); freedoc(d);
    }
    for(int i=0;i<100;i++){
        got=lsm_coll_get(lc,ids[i]);
        char exp[20]; snprintf(exp,20,"item-%d",i);
        assert(got && strcmp(got->pairs->value.str,exp)==0); freedoc(got);
    }
    lsm_coll_close(lc);
    printf("  PASS: 100 inserts\n");

    printf("All phase9 tests passed.\n");
    return 0;
}
