#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/core/lsm/memtable.h"

#define OK(expr) do{ int _r=(expr); if(_r!=LSM_OK){fprintf(stderr,"FAIL %s line %d ret=%d\n",#expr,__LINE__,_r);exit(1);}printf("  PASS: %s\n",#expr);}while(0)
#define EQ(a,b)  assert((a)==(b))

int main(void){
    printf("=== Phase 1: Memtable ===\n");

    /* basic put/get */
    Memtable *mt=memtable_create(); assert(mt);
    OK(memtable_put(mt,"apple",5,"red",3,1));
    OK(memtable_put(mt,"banana",6,"yellow",6,2));
    const void *v; size_t vl;
    EQ(memtable_get(mt,"banana",6,&v,&vl),LSM_OK);
    assert(vl==6 && memcmp(v,"yellow",6)==0);
    memtable_destroy(mt);

    /* overwrite: higher seq wins */
    mt=memtable_create();
    memtable_put(mt,"key",3,"v1",2,1);
    memtable_put(mt,"key",3,"v2",2,2);
    EQ(memtable_get(mt,"key",3,&v,&vl),LSM_OK);
    assert(memcmp(v,"v2",2)==0);
    memtable_destroy(mt);

    /* tombstone */
    mt=memtable_create();
    memtable_put(mt,"k",1,"val",3,1);
    memtable_delete(mt,"k",1,2);
    EQ(memtable_get(mt,"k",1,&v,&vl),LSM_DELETED);
    memtable_destroy(mt);

    /* not found */
    mt=memtable_create();
    memtable_put(mt,"a",1,"v",1,1);
    EQ(memtable_get(mt,"z",1,&v,&vl),LSM_NOT_FOUND);
    memtable_destroy(mt);

    /* iterator sorted */
    mt=memtable_create();
    memtable_put(mt,"z",1,"v",1,1);
    memtable_put(mt,"a",1,"v",1,2);
    memtable_put(mt,"m",1,"v",1,3);
    MemtableIterator *it=memtable_iter_new(mt);
    char prev[2]={0}; int cnt=0;
    while(memtable_iter_valid(it)){
        const MemtableEntry *e=memtable_iter_entry(it);
        char cur[2]={(char)entry_key(e)[0],0};
        if(cnt>0) assert(strcmp(prev,cur)<=0);
        prev[0]=cur[0]; cnt++;
        memtable_iter_next(it);
    }
    memtable_iter_free(it);
    assert(cnt>=3);
    memtable_destroy(mt);

    /* flush trigger */
    mt=memtable_create();
    char big[1025]; memset(big,'x',sizeof(big));
    for(int i=0;i<5000&&!memtable_should_flush(mt);i++){
        char key[16]; snprintf(key,16,"k%d",i);
        memtable_put(mt,key,strlen(key),big,sizeof(big),(uint64_t)i);
    }
    assert(memtable_should_flush(mt));
    memtable_destroy(mt);

    printf("All phase1 tests passed.\n");
    return 0;
}
