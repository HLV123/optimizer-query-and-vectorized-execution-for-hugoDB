#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/core/lsm/lsm.h"

#define DIR1 "/tmp/lsm_t1"
#define DIR2 "/tmp/lsm_t2"
#define DIR3 "/tmp/lsm_t3"
#define DIR4 "/tmp/lsm_t4"

static void rmrf(const char *d){
    char cmd[300]; snprintf(cmd,sizeof(cmd),"rm -rf %s",d); system(cmd);
}

static void test_basic(void){
    rmrf(DIR1);
    Lsm *lsm=lsm_open(DIR1); assert(lsm);
    assert(lsm_put(lsm,"key1",4,"value1",6)==LSM_OK);
    assert(lsm_put(lsm,"key2",4,"value2",6)==LSM_OK);
    assert(lsm_delete(lsm,"key1",4)==LSM_OK);
    void *v; size_t vl;
    assert(lsm_get(lsm,"key1",4,&v,&vl)==LSM_NOT_FOUND);
    assert(lsm_get(lsm,"key2",4,&v,&vl)==LSM_OK);
    assert(vl==6 && memcmp(v,"value2",6)==0); free(v);
    assert(lsm_get(lsm,"missing",7,&v,&vl)==LSM_NOT_FOUND);
    lsm_close(lsm);
    printf("  PASS: basic lifecycle\n");
}

static void test_persist(void){
    rmrf(DIR2);
    Lsm *lsm=lsm_open(DIR2); assert(lsm);
    lsm_put(lsm,"persistent",10,"data",4);
    lsm_close(lsm);
    lsm=lsm_open(DIR2); assert(lsm);
    void *v; size_t vl;
    assert(lsm_get(lsm,"persistent",10,&v,&vl)==LSM_OK);
    assert(vl==4 && memcmp(v,"data",4)==0); free(v);
    lsm_close(lsm);
    printf("  PASS: persist across restart\n");
}

static void manual_flush(Lsm *lsm){
    if(lsm->memtable->n_entries==0) return;
    lsm->imm=lsm->memtable;
    snprintf(lsm->imm_wal_path,sizeof(lsm->imm_wal_path),"%s",lsm->wal_path);
    lsm_wal_close(&lsm->wal);
    lsm->memtable=memtable_create();
    uint64_t wid=manifest_next_file_id(&lsm->manifest);
    lsm->manifest.current_wal_id=wid;
    char wp[300];
    snprintf(wp,sizeof(wp),"%s/wal/%06llu.wal",lsm->dir_path,(unsigned long long)wid);
    snprintf(lsm->wal_path,sizeof(lsm->wal_path),"%s",wp);
    lsm_wal_open(&lsm->wal,lsm->wal_path);
    lsm->wal.next_seq=lsm->manifest.next_seq_num;
    lsm_flush_memtable(lsm);
}

static void test_flush(void){
    rmrf(DIR3);
    Lsm *lsm=lsm_open(DIR3); assert(lsm);
    for(int i=0;i<500;i++){
        char key[20],val[20];
        snprintf(key,20,"key%06d",i);
        snprintf(val,20,"val%06d",i);
        lsm_put(lsm,key,strlen(key),val,strlen(val));
    }
    manual_flush(lsm);
    assert(lsm->manifest.level_count[0]>=1);
    void *v; size_t vl;
    assert(lsm_get(lsm,"key000250",9,&v,&vl)==LSM_OK); free(v);
    lsm_close(lsm);
    printf("  PASS: flush creates SSTable\n");
}

static void test_compaction(void){
    rmrf(DIR4);
    Lsm *lsm=lsm_open(DIR4); assert(lsm);
    /* Insert 3 batches < trigger, accumulate L0 */
    for(int batch=0;batch<3;batch++){
        for(int i=0;i<80;i++){
            char key[20]; snprintf(key,20,"k%d-%03d",batch,i);
            lsm_put(lsm,key,strlen(key),"v",1);
        }
        manual_flush(lsm);
    }
    /* Force compact whatever is in L0 */
    if(lsm->manifest.level_count[0]>0) lsm_compact_l0(lsm);
    assert(lsm->manifest.level_count[0]==0);
    assert(lsm->manifest.level_count[1]>=1);
    /* All keys still readable */
    for(int batch=0;batch<3;batch++){
        for(int i=0;i<80;i++){
            char key[20]; snprintf(key,20,"k%d-%03d",batch,i);
            void *v; size_t vl;
            assert(lsm_get(lsm,key,strlen(key),&v,&vl)==LSM_OK); free(v);
        }
    }
    lsm_close(lsm);
    printf("  PASS: L0 compaction\n");
}

int main(void){
    printf("=== Phase 5-8: LSM Lifecycle, Flush, Compaction ===\n");
    test_basic();
    test_persist();
    test_flush();
    test_compaction();
    printf("All phase5 tests passed.\n");
    return 0;
}
