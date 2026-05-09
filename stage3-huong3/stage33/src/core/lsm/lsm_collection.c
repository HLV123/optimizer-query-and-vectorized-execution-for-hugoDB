#include "lsm_collection.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void id_to_key(uint64_t id, uint8_t k[8]){
    k[0]=(uint8_t)(id>>56);k[1]=(uint8_t)(id>>48);
    k[2]=(uint8_t)(id>>40);k[3]=(uint8_t)(id>>32);
    k[4]=(uint8_t)(id>>24);k[5]=(uint8_t)(id>>16);
    k[6]=(uint8_t)(id>>8); k[7]=(uint8_t)id;
}
uint64_t key_to_id(const uint8_t k[8]){
    return ((uint64_t)k[0]<<56)|((uint64_t)k[1]<<48)|
           ((uint64_t)k[2]<<40)|((uint64_t)k[3]<<32)|
           ((uint64_t)k[4]<<24)|((uint64_t)k[5]<<16)|
           ((uint64_t)k[6]<<8)|(uint64_t)k[7];
}

/* Serialize: pairs separated by \0, each pair "key\x01T\x01val" */
int doc_serialize(const Document *doc, uint8_t **out, size_t *out_len){
    if(!doc||!out||!out_len) return LSM_ERR_IO;
    uint8_t *buf=NULL; size_t len=0,cap=0;
#define APPEND(data,dlen) do{ \
    if(len+(dlen)>cap){cap=cap?cap*2:256;while(cap<len+(dlen))cap*=2; \
    buf=(uint8_t*)realloc(buf,cap);} \
    memcpy(buf+len,(data),(dlen));len+=(dlen);}while(0)
    KVPair *p=doc->pairs;
    while(p){
        APPEND(p->key,strlen(p->key));
        APPEND("\x01",1);
        char tv[320];
        if(p->value.type==VAL_NUM)  snprintf(tv,sizeof(tv),"N\x01%g",p->value.num);
        else if(p->value.type==VAL_BOOL) snprintf(tv,sizeof(tv),"B\x01%d",(int)(p->value.num!=0));
        else snprintf(tv,sizeof(tv),"S\x01%s",p->value.str);
        APPEND(tv,strlen(tv));
        APPEND("\x00",1);
        p=p->next;
    }
    *out=buf; *out_len=len; return LSM_OK;
}

Document *doc_deserialize(const uint8_t *buf, size_t len){
    Document *doc=(Document*)calloc(1,sizeof(*doc));
    if(!doc) return NULL;
    KVPair *tail=NULL;
    size_t pos=0;
    while(pos<len){
        size_t end=pos; while(end<len&&buf[end]!='\x00') end++;
        if(end==pos){pos++;continue;}
        char tmp[512]={0};
        size_t plen=end-pos; if(plen>=sizeof(tmp)) plen=sizeof(tmp)-1;
        memcpy(tmp,buf+pos,plen);
        char *s1=(char*)memchr(tmp,'\x01',plen); if(!s1){pos=end+1;continue;}
        *s1='\0';
        char *s2=(char*)memchr(s1+1,'\x01',plen-(s1-tmp)-1); if(!s2){pos=end+1;continue;}
        *s2='\0';
        KVPair *kv=(KVPair*)calloc(1,sizeof(*kv));
        if(!kv) break;
        snprintf(kv->key,sizeof(kv->key),"%s",tmp);
        char *type=s1+1, *val=s2+1;
        if(type[0]=='N'){kv->value.type=VAL_NUM;kv->value.num=atof(val);}
        else if(type[0]=='B'){kv->value.type=VAL_BOOL;kv->value.num=atof(val);}
        else{kv->value.type=VAL_STR;snprintf(kv->value.str,sizeof(kv->value.str),"%s",val);}
        if(!doc->pairs) doc->pairs=kv;
        if(tail) tail->next=kv;
        tail=kv; doc->count++;
        pos=end+1;
    }
    return doc;
}

static const uint8_t META_KEY[8]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

LsmCollection *lsm_coll_open(const char *base_dir, const char *name){
    LsmCollection *lc=(LsmCollection*)calloc(1,sizeof(*lc));
    if(!lc) return NULL;
    snprintf(lc->name,sizeof(lc->name),"%s",name);
    snprintf(lc->dir_path,sizeof(lc->dir_path),"%s/%s.lsm",base_dir,name);
    lc->lsm=lsm_open(lc->dir_path);
    if(!lc->lsm){free(lc);return NULL;}
    void *v; size_t vl;
    if(lsm_get(lc->lsm,META_KEY,8,&v,&vl)==LSM_OK&&vl>=8){
        lc->next_id=key_to_id((const uint8_t*)v); free(v);
    } else lc->next_id=1;
    return lc;
}

int lsm_coll_close(LsmCollection *lc){
    if(!lc) return LSM_OK;
    uint8_t kb[8]; id_to_key(lc->next_id,kb);
    lsm_put(lc->lsm,META_KEY,8,kb,8);
    lsm_close(lc->lsm); free(lc); return LSM_OK;
}

int lsm_coll_insert(LsmCollection *lc,Document *doc,uint64_t *out_id){
    if(!lc||!doc) return LSM_ERR_IO;
    uint64_t id=lc->next_id++;
    uint8_t *buf; size_t len;
    if(doc_serialize(doc,&buf,&len)!=LSM_OK) return LSM_ERR_NOMEM;
    uint8_t key[8]; id_to_key(id,key);
    int r=lsm_put(lc->lsm,key,8,buf,len);
    free(buf);
    if(r==LSM_OK&&out_id) *out_id=id;
    return r;
}

Document *lsm_coll_get(LsmCollection *lc, uint64_t id){
    if(!lc) return NULL;
    uint8_t key[8]; id_to_key(id,key);
    void *v; size_t vl;
    if(lsm_get(lc->lsm,key,8,&v,&vl)!=LSM_OK) return NULL;
    Document *d=doc_deserialize((const uint8_t*)v,vl);
    free(v); return d;
}

int lsm_coll_update(LsmCollection *lc,uint64_t id,Document *doc){
    if(!lc||!doc) return LSM_ERR_IO;
    uint8_t *buf; size_t len;
    if(doc_serialize(doc,&buf,&len)!=LSM_OK) return LSM_ERR_NOMEM;
    uint8_t key[8]; id_to_key(id,key);
    int r=lsm_put(lc->lsm,key,8,buf,len);
    free(buf); return r;
}

int lsm_coll_delete(LsmCollection *lc,uint64_t id){
    if(!lc) return LSM_ERR_IO;
    uint8_t key[8]; id_to_key(id,key);
    return lsm_delete(lc->lsm,key,8);
}

typedef struct{LsmCollScanFn fn;void*ctx;}ScanCtx;
static int scan_cb(const void*key,size_t kl,const void*val,size_t vl,
                   uint64_t seq,uint8_t op,void*ctx){
    (void)seq;
    if(kl!=8||op!=LSM_OP_PUT) return LSM_OK;
    uint64_t id=key_to_id((const uint8_t*)key);
    if(id==0xFFFFFFFFFFFFFFFFULL) return LSM_OK;
    ScanCtx *sc=(ScanCtx*)ctx;
    Document *d=doc_deserialize((const uint8_t*)val,vl);
    if(d){sc->fn(id,d,sc->ctx);}
    return LSM_OK;
}
int lsm_coll_scan(LsmCollection *lc,LsmCollScanFn fn,void*ctx){
    ScanCtx sc={fn,ctx};
    return lsm_scan(lc->lsm,scan_cb,&sc);
}
