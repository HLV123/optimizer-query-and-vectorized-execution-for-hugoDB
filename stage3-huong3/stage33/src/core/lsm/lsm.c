#include "lsm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_p(p) mkdir(p,0755)
#endif

static void ensure_dir(const char *p) {
    char tmp[512]; size_t len=strlen(p);
    if(len>=sizeof(tmp)){mkdir_p(p);return;}
    memcpy(tmp,p,len+1);
    for(size_t i=1;i<=len;i++){
        if(tmp[i]=='/'||tmp[i]=='\0'){
            char sv=tmp[i];tmp[i]='\0';
            mkdir_p(tmp);
            tmp[i]=sv;
        }
    }
}

static void wal_path(const char *dir, uint64_t id, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/wal/%06llu.wal", dir, (unsigned long long)id);
}
static void sst_path(const char *dir, uint64_t id, int lv, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/sstables/%06llu-L%d.sst", dir, (unsigned long long)id, lv);
}

static int cmpk(const void *a,size_t al,const void *b,size_t bl){
    size_t mn=al<bl?al:bl; int r=memcmp(a,b,mn);
    if(r) return r; if(al<bl) return -1; if(al>bl) return 1; return 0;
}

/* ---- reader cache ---- */
static SSTableReader *get_reader(Lsm *lsm, SSTableMetadata *meta) {
    for (size_t i=0;i<lsm->n_readers;i++)
        if (lsm->readers[i] && lsm->readers[i]->meta &&
            lsm->readers[i]->meta->file_id == meta->file_id)
            return lsm->readers[i];
    SSTableReader *r = sstable_open(meta->path, meta);
    if (!r) return NULL;
    if (lsm->n_readers >= lsm->readers_cap) {
        size_t nc = lsm->readers_cap ? lsm->readers_cap*2 : 16;
        SSTableReader **nb = (SSTableReader **)realloc(lsm->readers, nc*sizeof(*nb));
        if (!nb) { sstable_close(r); return NULL; }
        lsm->readers=nb; lsm->readers_cap=nc;
    }
    lsm->readers[lsm->n_readers++]=r;
    return r;
}
static void evict_reader(Lsm *lsm, uint64_t file_id) {
    for (size_t i=0;i<lsm->n_readers;i++)
        if (lsm->readers[i] && lsm->readers[i]->meta &&
            lsm->readers[i]->meta->file_id==file_id) {
            sstable_close(lsm->readers[i]);
            lsm->readers[i]=lsm->readers[--lsm->n_readers];
            return;
        }
}

/* ---- WAL replay ---- */
typedef struct { Lsm *lsm; uint64_t max_seq; } ReplayCtx;
static int replay_cb(uint64_t seq,uint8_t op,const void*k,size_t kl,
                     const void*v,size_t vl,void*ctx){
    ReplayCtx *rc=(ReplayCtx*)ctx;
    if(op==LSM_OP_PUT) memtable_put(rc->lsm->memtable,k,kl,v,vl,seq);
    else               memtable_delete(rc->lsm->memtable,k,kl,seq);
    if(seq>rc->max_seq) rc->max_seq=seq;
    return LSM_OK;
}

/* ---- open ---- */
Lsm *lsm_open(const char *dir) {
    Lsm *lsm=(Lsm*)calloc(1,sizeof(*lsm));
    if(!lsm) return NULL;
    snprintf(lsm->dir_path,sizeof(lsm->dir_path),"%s",dir);
    char sub[300];
    ensure_dir(dir);
    snprintf(sub,sizeof(sub),"%s/wal",dir);      ensure_dir(sub);
    snprintf(sub,sizeof(sub),"%s/sstables",dir); ensure_dir(sub);

    if(manifest_load(&lsm->manifest,dir)!=LSM_OK){free(lsm);return NULL;}

    lsm->memtable=memtable_create();
    if(!lsm->memtable){manifest_destroy(&lsm->manifest);free(lsm);return NULL;}

    uint64_t wid=lsm->manifest.current_wal_id;
    if(!wid){ wid=manifest_next_file_id(&lsm->manifest); lsm->manifest.current_wal_id=wid; }
    wal_path(dir,wid,lsm->wal_path,sizeof(lsm->wal_path));
    lsm_wal_open(&lsm->wal,lsm->wal_path);
    lsm->wal.next_seq=lsm->manifest.next_seq_num;

    ReplayCtx rc={lsm,0};
    lsm_wal_replay(lsm->wal_path,replay_cb,&rc);
    if(rc.max_seq>=lsm->manifest.next_seq_num){
        lsm->manifest.next_seq_num=rc.max_seq+1;
        lsm->wal.next_seq=lsm->manifest.next_seq_num;
    }
    return lsm;
}

static void rotate_memtable(Lsm *lsm){
    lsm->imm=lsm->memtable;
    snprintf(lsm->imm_wal_path,sizeof(lsm->imm_wal_path),"%s",lsm->wal_path);
    lsm_wal_close(&lsm->wal);
    lsm->memtable=memtable_create();
    uint64_t wid=manifest_next_file_id(&lsm->manifest);
    lsm->manifest.current_wal_id=wid;
    wal_path(lsm->dir_path,wid,lsm->wal_path,sizeof(lsm->wal_path));
    lsm_wal_open(&lsm->wal,lsm->wal_path);
    lsm->wal.next_seq=lsm->manifest.next_seq_num;
}

/* ---- close ---- */
int lsm_close(Lsm *lsm){
    if(!lsm) return LSM_OK;
    if(lsm->memtable && lsm->memtable->n_entries>0){
        rotate_memtable(lsm);
        lsm_flush_memtable(lsm);
    }
    lsm_wal_close(&lsm->wal);
    manifest_save(&lsm->manifest);
    for(size_t i=0;i<lsm->n_readers;i++) if(lsm->readers[i]) sstable_close(lsm->readers[i]);
    free(lsm->readers);
    memtable_destroy(lsm->memtable);
    memtable_destroy(lsm->imm);
    manifest_destroy(&lsm->manifest);
    free(lsm);
    return LSM_OK;
}

/* ---- put ---- */
int lsm_put(Lsm *lsm,const void*key,size_t kl,const void*val,size_t vl){
    if(!lsm||!key||!kl) return LSM_ERR_IO;
    uint64_t seq;
    if(lsm_wal_append(&lsm->wal,LSM_OP_PUT,key,kl,val,vl,&seq)!=LSM_OK) return LSM_ERR_IO;
    lsm->manifest.next_seq_num=lsm->wal.next_seq;
    lsm_wal_sync(&lsm->wal);
    memtable_put(lsm->memtable,key,kl,val,vl,seq);
    if(memtable_should_flush(lsm->memtable)){
        rotate_memtable(lsm);
        lsm_flush_memtable(lsm);
    }
    return LSM_OK;
}

/* ---- delete ---- */
int lsm_delete(Lsm *lsm,const void*key,size_t kl){
    if(!lsm||!key||!kl) return LSM_ERR_IO;
    uint64_t seq;
    lsm_wal_append(&lsm->wal,LSM_OP_DELETE,key,kl,NULL,0,&seq);
    lsm->manifest.next_seq_num=lsm->wal.next_seq;
    lsm_wal_sync(&lsm->wal);
    return memtable_delete(lsm->memtable,key,kl,seq);
}

/* ---- get ---- */
static bool key_in_range(const void*key,size_t kl,const SSTableMetadata*m){
    if(!m->min_key||!m->max_key) return true;
    return cmpk(key,kl,m->min_key,m->min_key_len)>=0 &&
           cmpk(key,kl,m->max_key,m->max_key_len)<=0;
}

int lsm_get(Lsm *lsm,const void*key,size_t kl,void**out_val,size_t*out_vl){
    if(!lsm||!key||!kl) return LSM_NOT_FOUND;
    const void *v; size_t vl; int ret;

    ret=memtable_get(lsm->memtable,key,kl,&v,&vl);
    if(ret==LSM_OK){
        if(out_val){*out_val=malloc(vl+1);memcpy(*out_val,v,vl);((char*)*out_val)[vl]='\0';}
        if(out_vl) *out_vl=vl;
        return LSM_OK;
    }
    if(ret==LSM_DELETED) return LSM_NOT_FOUND;

    if(lsm->imm){
        ret=memtable_get(lsm->imm,key,kl,&v,&vl);
        if(ret==LSM_OK){
            if(out_val){*out_val=malloc(vl+1);memcpy(*out_val,v,vl);((char*)*out_val)[vl]='\0';}
            if(out_vl) *out_vl=vl;
            return LSM_OK;
        }
        if(ret==LSM_DELETED) return LSM_NOT_FOUND;
    }

    /* L0 newest first */
    LsmManifest *mf=&lsm->manifest;
    for(int i=(int)mf->level_count[0]-1;i>=0;i--){
        SSTableMetadata *m=mf->levels[0][i];
        if(!key_in_range(key,kl,m)) continue;
        SSTableReader *r=get_reader(lsm,m);
        if(!r) continue;
        void *rv=NULL; size_t rvl=0;
        ret=sstable_get(r,key,kl,&rv,&rvl,NULL);
        if(ret==LSM_OK){if(out_val)*out_val=rv;else free(rv);if(out_vl)*out_vl=rvl;return LSM_OK;}
        if(ret==LSM_DELETED) return LSM_NOT_FOUND;
    }

    /* L1+ binary search */
    for(int lv=1;lv<LSM_MAX_LEVELS;lv++){
        if(!mf->level_count[lv]) continue;
        /* find SSTable where min_key <= key <= max_key */
        SSTableMetadata *found=NULL;
        for(size_t i=0;i<mf->level_count[lv];i++){
            SSTableMetadata *m=mf->levels[lv][i];
            if(key_in_range(key,kl,m)){found=m;break;}
        }
        if(!found) continue;
        SSTableReader *r=get_reader(lsm,found);
        if(!r) continue;
        void *rv=NULL; size_t rvl=0;
        ret=sstable_get(r,key,kl,&rv,&rvl,NULL);
        if(ret==LSM_OK){if(out_val)*out_val=rv;else free(rv);if(out_vl)*out_vl=rvl;return LSM_OK;}
        if(ret==LSM_DELETED) return LSM_NOT_FOUND;
    }
    return LSM_NOT_FOUND;
}

/* ---- flush ---- */
int lsm_flush_memtable(Lsm *lsm){
    if(!lsm->imm||lsm->imm->n_entries==0){
        if(lsm->imm){memtable_destroy(lsm->imm);lsm->imm=NULL;}
        return LSM_OK;
    }
    uint64_t fid=manifest_next_file_id(&lsm->manifest);
    char path[300]; sst_path(lsm->dir_path,fid,0,path,sizeof(path));
    SSTableBuilder *b=sstable_builder_new(path,lsm->imm->n_entries);
    if(!b) return LSM_ERR_NOMEM;
    MemtableIterator *it=memtable_iter_new(lsm->imm);
    while(memtable_iter_valid(it)){
        sstable_builder_add(b,memtable_iter_entry(it));
        memtable_iter_next(it);
    }
    memtable_iter_free(it);
    SSTableMetadata *meta=(SSTableMetadata*)calloc(1,sizeof(*meta));
    if(!meta){sstable_builder_abandon(b);return LSM_ERR_NOMEM;}
    int r=sstable_builder_finish(b,meta);
    if(r!=LSM_OK){free(meta);return r;}
    meta->file_id=fid; meta->level=0;
    manifest_add_sstable(&lsm->manifest,0,meta);
    manifest_save(&lsm->manifest);
    memtable_destroy(lsm->imm); lsm->imm=NULL;
    lsm_wal_delete(lsm->imm_wal_path); lsm->imm_wal_path[0]='\0';
    if(lsm->manifest.level_count[0]>=LSM_L0_TRIGGER) lsm_compact_l0(lsm);
    return LSM_OK;
}

/* ---- k-way merge heap ---- */
typedef struct { SSTableIterator *it; int src; } HItem;
typedef struct { HItem *a; size_t n, cap; } Heap;

static int hcmp(const HItem *a,const HItem *b){
    const MemtableEntry *ea=sstable_iter_entry(a->it);
    const MemtableEntry *eb=sstable_iter_entry(b->it);
    int c=cmpk(entry_key(ea),ea->key_len,entry_key(eb),eb->key_len);
    if(c) return c;
    return ea->seq_num>eb->seq_num?-1:ea->seq_num<eb->seq_num?1:0;
}
static void hpush(Heap *h,HItem x){
    if(h->n>=h->cap){
        h->cap=h->cap?h->cap*2:16;
        h->a=(HItem*)realloc(h->a,h->cap*sizeof(HItem));
    }
    h->a[h->n++]=x;
    size_t i=h->n-1;
    while(i>0){size_t p=(i-1)/2;if(hcmp(&h->a[p],&h->a[i])>0){HItem t=h->a[p];h->a[p]=h->a[i];h->a[i]=t;i=p;}else break;}
}
static HItem hpop(Heap *h){
    HItem top=h->a[0]; h->a[0]=h->a[--h->n];
    size_t i=0;
    while(1){size_t l=2*i+1,r=2*i+2,s=i;
        if(l<h->n&&hcmp(&h->a[l],&h->a[s])<0)s=l;
        if(r<h->n&&hcmp(&h->a[r],&h->a[s])<0)s=r;
        if(s==i)break;
        HItem t=h->a[i];h->a[i]=h->a[s];h->a[s]=t;i=s;}
    return top;
}

static int do_merge(Lsm *lsm,
                    SSTableMetadata **in_files, size_t n_in, int in_level,
                    int out_level, bool is_bottom){
    Heap heap={NULL,0,0};
    SSTableIterator **iters=(SSTableIterator**)calloc(n_in,sizeof(*iters));
    if(!iters) return LSM_ERR_NOMEM;
    size_t est=0;
    for(size_t i=0;i<n_in;i++){
        est+=in_files[i]->num_entries;
        SSTableReader *r=get_reader(lsm,in_files[i]);
        if(r){iters[i]=sstable_iter_new(r);
            if(sstable_iter_valid(iters[i])){HItem hi={iters[i],(int)i};hpush(&heap,hi);}}
    }

    uint64_t oid=manifest_next_file_id(&lsm->manifest);
    char opath[300]; sst_path(lsm->dir_path,oid,out_level,opath,sizeof(opath));
    SSTableBuilder *bldr=sstable_builder_new(opath,est);
    if(!bldr){free(iters);free(heap.a);return LSM_ERR_NOMEM;}

    char last_key[4096]; size_t last_kl=0; uint64_t last_seq=0; bool have_last=false;
    while(heap.n>0){
        HItem hi=hpop(&heap);
        const MemtableEntry *e=sstable_iter_entry(hi.it);
        if(!e) goto adv;
        {
            bool same=have_last && last_kl==e->key_len &&
                      memcmp(last_key,entry_key(e),e->key_len)==0;
            if(same && e->seq_num<=last_seq) goto adv;
            if(e->op_type==LSM_OP_DELETE && is_bottom) goto adv;
            sstable_builder_add(bldr,e);
            if(!same||e->seq_num>last_seq){
                if(e->key_len<sizeof(last_key)){
                    memcpy(last_key,entry_key(e),e->key_len);
                    last_kl=e->key_len; last_seq=e->seq_num;
                }
                have_last=true;
            }
        }
adv:    sstable_iter_next(hi.it);
        if(sstable_iter_valid(hi.it)) hpush(&heap,hi);
    }
    for(size_t i=0;i<n_in;i++) if(iters[i]) sstable_iter_free(iters[i]);
    free(iters); free(heap.a);

    SSTableMetadata *out=(SSTableMetadata*)calloc(1,sizeof(*out));
    if(!out){sstable_builder_abandon(bldr);return LSM_ERR_NOMEM;}
    int r=sstable_builder_finish(bldr,out);
    if(r!=LSM_OK){free(out);return r;}
    out->file_id=oid; out->level=out_level;

    /* collect paths to delete */
    char del[64][300]; size_t ndel=0;
    for(size_t i=0;i<n_in&&ndel<64;i++){
        evict_reader(lsm,in_files[i]->file_id);
        snprintf(del[ndel++],300,"%s",in_files[i]->path);
        manifest_remove_sstable(&lsm->manifest,in_level,in_files[i]->file_id);
    }
    manifest_add_sstable(&lsm->manifest,out_level,out);
    manifest_save(&lsm->manifest);
    for(size_t i=0;i<ndel;i++) remove(del[i]);
    return LSM_OK;
}

int lsm_compact_l0(Lsm *lsm){
    LsmManifest *mf=&lsm->manifest;
    if(!mf->level_count[0]) return LSM_OK;
    bool is_bottom=(mf->level_count[1]==0&&mf->level_count[2]==0&&mf->level_count[3]==0);
    /* copy pointers since do_merge will remove them */
    size_t n=mf->level_count[0];
    SSTableMetadata **files=(SSTableMetadata**)malloc(n*sizeof(*files));
    if(!files) return LSM_ERR_NOMEM;
    for(size_t i=0;i<n;i++) files[i]=mf->levels[0][i];
    int r=do_merge(lsm,files,n,0,1,is_bottom);
    free(files); return r;
}

int lsm_compact_level(Lsm *lsm,int level){
    if(level<1||level>=LSM_MAX_LEVELS-1) return LSM_ERR_IO;
    static const uint64_t LIMIT[]={0,
        10ULL<<20, 100ULL<<20, 1000ULL<<20,
        10000ULL<<20, 100000ULL<<20, 0};
    LsmManifest *mf=&lsm->manifest;
    if(!mf->level_count[level]) return LSM_OK;
    uint64_t sz=0;
    for(size_t i=0;i<mf->level_count[level];i++) sz+=mf->levels[level][i]->file_size;
    if(LIMIT[level]&&sz<=LIMIT[level]) return LSM_OK;

    /* pick first file + overlapping at level+1 */
    SSTableMetadata *src=mf->levels[level][0];
    SSTableMetadata *overlap[64]; size_t noverlap=0;
    for(size_t i=0;i<mf->level_count[level+1]&&noverlap<64;i++){
        SSTableMetadata *m=mf->levels[level+1][i];
        if(cmpk(src->min_key,src->min_key_len,m->max_key,m->max_key_len)<=0 &&
           cmpk(m->min_key,m->min_key_len,src->max_key,src->max_key_len)<=0)
            overlap[noverlap++]=m;
    }

    /* build input array */
    size_t total=1+noverlap;
    SSTableMetadata **files=(SSTableMetadata**)malloc(total*sizeof(*files));
    if(!files) return LSM_ERR_NOMEM;
    files[0]=src;
    for(size_t i=0;i<noverlap;i++) files[1+i]=overlap[i];
    bool is_bottom=(level+1==LSM_MAX_LEVELS-1);
    /* remove overlap from level+1 before merge (do_merge removes by in_level) */
    /* we pass level for src; overlap files need separate handling */
    /* Simpler: just merge all into level+1, remove old files manually */
    (void)is_bottom;

    /* merge src + overlap -> level+1 */
    int r=do_merge(lsm,files,total,level,level+1,is_bottom);
    free(files);
    return r;
}

/* ---- scan ---- */
int lsm_scan(Lsm *lsm,LsmScanFn fn,void *ctx){
    if(!lsm||!fn) return LSM_ERR_IO;
    MemtableIterator *it=memtable_iter_new(lsm->memtable);
    while(memtable_iter_valid(it)){
        const MemtableEntry *e=memtable_iter_entry(it);
        fn(entry_key(e),e->key_len,entry_value(e),e->value_len,e->seq_num,e->op_type,ctx);
        memtable_iter_next(it);
    }
    memtable_iter_free(it);
    LsmManifest *mf=&lsm->manifest;
    for(int lv=0;lv<LSM_MAX_LEVELS;lv++){
        for(size_t i=0;i<mf->level_count[lv];i++){
            SSTableReader *r=get_reader(lsm,mf->levels[lv][i]);
            if(!r) continue;
            SSTableIterator *sit=sstable_iter_new(r);
            while(sstable_iter_valid(sit)){
                const MemtableEntry *e=sstable_iter_entry(sit);
                fn(entry_key(e),e->key_len,entry_value(e),e->value_len,e->seq_num,e->op_type,ctx);
                sstable_iter_next(sit);
            }
            sstable_iter_free(sit);
        }
    }
    return LSM_OK;
}
