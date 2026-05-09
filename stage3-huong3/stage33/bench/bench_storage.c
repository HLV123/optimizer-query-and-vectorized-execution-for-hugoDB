/* bench_storage.c — LSM benchmark (Phase 10) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/core/lsm/lsm.h"

#define N_OPS    10000
#define VAL_SIZE 64

#ifdef _WIN32
#include <windows.h>
static double now_sec(void){
    LARGE_INTEGER freq,cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart/(double)freq.QuadPart;
}
#else
#include <time.h>
static double now_sec(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec+ts.tv_nsec*1e-9;
}
#endif

static void bench_lsm_seq_write(void){
    Lsm *lsm=lsm_open("bench_sw"); assert(lsm);
    char val[VAL_SIZE]; memset(val,'x',sizeof(val));
    double t0=now_sec();
    for(int i=0;i<N_OPS;i++){
        char key[24]; snprintf(key,24,"key%09d",i);
        lsm_put(lsm,key,strlen(key),val,sizeof(val));
    }
    lsm_close(lsm);
    double t1=now_sec();
    printf("LSM seq_write  %d ops: %.3f s  (%.0f ops/s)\n",N_OPS,t1-t0,N_OPS/(t1-t0));
}

static void bench_lsm_rand_write(void){
    Lsm *lsm=lsm_open("bench_rw"); assert(lsm);
    char val[VAL_SIZE]; memset(val,'y',sizeof(val));
    double t0=now_sec();
    for(int i=0;i<N_OPS;i++){
        unsigned r=(unsigned)rand()%N_OPS;
        char key[24]; snprintf(key,24,"key%09u",r);
        lsm_put(lsm,key,strlen(key),val,sizeof(val));
    }
    lsm_close(lsm);
    double t1=now_sec();
    printf("LSM rand_write %d ops: %.3f s  (%.0f ops/s)\n",N_OPS,t1-t0,N_OPS/(t1-t0));
}

static void bench_lsm_seq_read(void){
    Lsm *lsm=lsm_open("bench_sr"); assert(lsm);
    char val[VAL_SIZE]; memset(val,'z',sizeof(val));
    for(int i=0;i<N_OPS;i++){
        char key[24]; snprintf(key,24,"key%09d",i);
        lsm_put(lsm,key,strlen(key),val,sizeof(val));
    }
    double t0=now_sec(); int hits=0;
    for(int i=0;i<N_OPS;i++){
        char key[24]; snprintf(key,24,"key%09d",i);
        void *v; size_t vl;
        if(lsm_get(lsm,key,strlen(key),&v,&vl)==LSM_OK){hits++;free(v);}
    }
    double t1=now_sec();
    lsm_close(lsm);
    printf("LSM seq_read   %d ops: %.3f s  (%.0f ops/s) hits=%d\n",N_OPS,t1-t0,N_OPS/(t1-t0),hits);
}

static void bench_lsm_rand_read(void){
    Lsm *lsm=lsm_open("bench_rr"); assert(lsm);
    char val[VAL_SIZE]; memset(val,'w',sizeof(val));
    for(int i=0;i<N_OPS;i++){
        char key[24]; snprintf(key,24,"key%09d",i);
        lsm_put(lsm,key,strlen(key),val,sizeof(val));
    }
    double t0=now_sec(); int hits=0;
    for(int i=0;i<N_OPS;i++){
        unsigned r=(unsigned)rand()%N_OPS;
        char key[24]; snprintf(key,24,"key%09u",r);
        void *v; size_t vl;
        if(lsm_get(lsm,key,strlen(key),&v,&vl)==LSM_OK){hits++;free(v);}
    }
    double t1=now_sec();
    lsm_close(lsm);
    printf("LSM rand_read  %d ops: %.3f s  (%.0f ops/s) hits=%d\n",N_OPS,t1-t0,N_OPS/(t1-t0),hits);
}

int main(void){
    srand(42);
    printf("=== LSM Benchmark (%d ops each) ===\n\n",N_OPS);
    bench_lsm_seq_write();
    bench_lsm_rand_write();
    bench_lsm_seq_read();
    bench_lsm_rand_read();
    printf("\nDone.\n");
    return 0;
}
