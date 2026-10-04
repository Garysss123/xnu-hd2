#include "leo_log_locator.h"
#include <string.h>
#define EOC 0x0ffffff8U
#define LAST_DATA 0x0fffffefU
struct volume { uint64_t part,total,data,fat[2]; uint32_t clusters,root,spf; unsigned spc,nfats,active,mirror; };
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int power2(uint32_t n){return n && !(n&(n-1));}
static int read_sector(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,uint64_t lba,uint8_t *dst){
 if(lba>=io->sectors)return FL32_RANGE;
 if(w->reads>=lim->read_budget)return FL32_LIMIT;
 w->reads++;int r=io->read512(io->cookie,lba,dst);
 if(r){w->backend_error=r;return FL32_IO;}return FL32_OK;
}
static int data_cluster(const struct volume *v,uint32_t c){return c>=2 && c<=LAST_DATA && (uint64_t)c<v->clusters+2ULL;}
static int cluster_lba(const struct volume *v,uint32_t c,uint64_t *lba){
 if(!data_cluster(v,c))return FL32_CHAIN;
 uint64_t p=v->data+(uint64_t)(c-2)*v->spc;
 if(p<v->data || p>=v->part+v->total || v->spc>v->part+v->total-p)return FL32_RANGE;
 *lba=p;return FL32_OK;
}
static int volume_read(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,struct volume *v){
 int r=read_sector(io,lim,w,0,w->sector);if(r)return r;
 if(w->sector[510]!=0x55 || w->sector[511]!=0xaa)return FL32_FORMAT;
 uint64_t starts[4],ends[4];unsigned used=0,found=0;uint64_t chosen_start=0,chosen_size=0;
 for(unsigned i=0;i<4;i++){
  const uint8_t *e=w->sector+446+16*i;uint32_t start=le32(e+8),size=le32(e+12);
  if(!e[4]){if(start || size)return FL32_FORMAT;continue;}
  if(e[0]!=0 && e[0]!=0x80)return FL32_FORMAT;
  if(!start || !size || (uint64_t)start+size>io->sectors)return FL32_RANGE;
  starts[used]=start;ends[used]=(uint64_t)start+size;
  for(unsigned j=0;j<used;j++)if(starts[used]<ends[j] && starts[j]<ends[used])return FL32_RANGE;
  used++;
  if(e[4]==0x0b || e[4]==0x0c){chosen_start=start;chosen_size=size;found++;}
 }
 if(found!=1)return FL32_FORMAT;
 r=read_sector(io,lim,w,chosen_start,w->sector);if(r)return r;
 const uint8_t *b=w->sector;unsigned spc=b[13],nf=b[16];uint32_t reserved=le16(b+14),spf=le32(b+36),total=le32(b+32),root=le32(b+44);uint16_t flags=le16(b+40);
 if(b[510]!=0x55 || b[511]!=0xaa || le16(b+11)!=512 || !power2(spc) || spc>128 || !reserved || !spf || (nf!=1 && nf!=2) || le16(b+17) || le16(b+19) || le16(b+22) || le16(b+42) || (flags&0xff70U) || le32(b+28)!=chosen_start)return FL32_FORMAT;
 uint64_t meta=(uint64_t)reserved+(uint64_t)nf*spf;
 if(!total || total>chosen_size || meta>=total)return FL32_RANGE;
 uint64_t clusters=(total-meta)/spc;
 if(clusters<65525 || clusters>LAST_DATA-1ULL || (uint64_t)spf*128<clusters+2)return FL32_FORMAT;
 v->part=chosen_start;v->total=total;v->data=chosen_start+meta;v->spf=spf;v->spc=spc;v->nfats=nf;v->mirror=!(flags&0x80U);v->active=v->mirror?0U:(flags&15U);v->clusters=(uint32_t)clusters;v->root=root;
 if(v->active>=nf || !data_cluster(v,root))return FL32_FORMAT;
 for(unsigned i=0;i<nf;i++)v->fat[i]=chosen_start+reserved+(uint64_t)i*spf;
 return FL32_OK;
}
static int fat_one(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,unsigned f,uint32_t c,uint32_t *next){
 uint64_t index=c/128U;if(f>=v->nfats || index>=v->spf)return FL32_RANGE;
 uint64_t lba=v->fat[f]+index;
 if(w->fat_lba[f]!=lba){int r=read_sector(io,lim,w,lba,w->fat[f]);if(r)return r;w->fat_lba[f]=lba;}
 *next=le32(w->fat[f]+(c%128U)*4U)&0x0fffffffU;return FL32_OK;
}
static int fat_next(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,uint32_t c,uint32_t *next){
 if(!data_cluster(v,c))return FL32_CHAIN;
 uint32_t a,b;int r=fat_one(io,lim,w,v,v->active,c,&a);if(r)return r;
 if(v->mirror && v->nfats==2){r=fat_one(io,lim,w,v,1,c,&b);if(r)return r;if(a!=b)return FL32_MIRROR;}
 if(a<EOC && !data_cluster(v,a))return FL32_CHAIN;
 *next=a;return FL32_OK;
}
static int seen(const struct fl32_work *w,unsigned n,uint32_t c){for(unsigned i=0;i<n;i++)if(w->seen_directory[i]==c)return 1;return 0;}
static int find_name(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,uint32_t first,const uint8_t name[11],int isdir,unsigned *all_seen,uint32_t *entry_cluster,uint32_t *entry_size){
 uint32_t c=first;unsigned nodes=0,matches=0;int ended=0;
 for(;;){
  if(!data_cluster(v,c) || seen(w,*all_seen,c))return FL32_CHAIN;
  if(nodes>=lim->directory_clusters || *all_seen>=FL32_DIRECTORY_CAP*2U)return FL32_LIMIT;
  w->seen_directory[(*all_seen)++]=c;nodes++;
  uint64_t lba;int r=cluster_lba(v,c,&lba);if(r)return r;
  if(!ended)for(unsigned s=0;s<v->spc && !ended;s++){
   r=read_sector(io,lim,w,lba+s,w->sector);if(r)return r;
   for(unsigned off=0;off<512;off+=32){const uint8_t *e=w->sector+off;
    if(!e[0]){ended=1;break;}
    if(e[0]==0xe5 || e[11]==0x0f || (e[11]&8))continue;
    if(memcmp(e,name,11))continue;
    if(++matches>1)return FL32_DUPLICATE;
    if((e[11]&0xc0) || (!!(e[11]&0x10)!=!!isdir))return FL32_FORMAT;
    *entry_cluster=((uint32_t)le16(e+20)<<16)|le16(e+26);*entry_size=le32(e+28);
    if(!data_cluster(v,*entry_cluster) || (isdir && *entry_size))return FL32_FORMAT;
   }
  }
  uint32_t next;r=fat_next(io,lim,w,v,c,&next);if(r)return r;
  if(next>=EOC)break;
  c=next;
 }
 return matches==1?FL32_OK:FL32_NOT_FOUND;
}
static int image_chain(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,uint32_t first,uint32_t bytes,unsigned dirs,struct fl32_extent *ext,uint32_t capacity,uint32_t *count){
 uint32_t c=first,remaining=bytes/512U,logical=0,n=0;
 uint64_t needed=((uint64_t)remaining+v->spc-1)/v->spc;
 for(uint64_t i=0;i<needed;i++){
  if(!data_cluster(v,c) || seen(w,dirs,c))return FL32_CHAIN;
  uint64_t physical;int r=cluster_lba(v,c,&physical);if(r)return r;
  uint32_t take=remaining<v->spc?remaining:v->spc;
  if(n && ext[n-1].physical_lba+ext[n-1].sectors==physical && ext[n-1].logical_lba+ext[n-1].sectors==logical)ext[n-1].sectors+=take;
  else {if(n>=capacity)return FL32_LIMIT;ext[n++]=(struct fl32_extent){physical,logical,take};}
  logical+=take;remaining-=take;
  uint32_t next;r=fat_next(io,lim,w,v,c,&next);if(r)return r;
  if(i+1==needed){if(next<EOC)return FL32_CHAIN;}
  else {if(next>=EOC)return FL32_CHAIN;c=next;}
 }
 if(remaining || logical!=bytes/512U)return FL32_CHAIN;
 *count=n;return FL32_OK;
}
static int translate_raw(const struct fl32_image *im,const struct fl32_extent *ex,uint32_t cap,uint64_t logical,uint64_t *physical,uint32_t *run){
 if(!im || !ex || !physical || !run || !im->extent_count || im->extent_count>cap || logical>=im->image_sectors)return FL32_ARGUMENT;
 uint32_t lo=0,hi=im->extent_count;
 while(lo<hi){uint32_t mid=lo+(hi-lo)/2;const struct fl32_extent *e=ex+mid;
  if(!e->sectors || (uint64_t)e->logical_lba+e->sectors>im->image_sectors)return FL32_RANGE;
  if(logical<e->logical_lba)hi=mid;
  else if(logical>=(uint64_t)e->logical_lba+e->sectors)lo=mid+1;
  else {uint64_t delta=logical-e->logical_lba;if(e->physical_lba>UINT64_MAX-delta)return FL32_RANGE;uint64_t p=e->physical_lba+delta;
   if(p<im->data_lba || p>=im->card_sectors || p<im->partition_lba || p-im->partition_lba>=im->partition_sectors)return FL32_RANGE;
   uint32_t available=e->sectors-(uint32_t)delta;
   if(available>im->card_sectors-p || available>im->partition_sectors-(p-im->partition_lba))return FL32_RANGE;
   *physical=p;*run=available;return FL32_OK;}
 }
 return FL32_RANGE;
}
static int own_cluster(uint8_t *bits,uint32_t bytes,const struct volume *v,uint32_t c){
 if(!data_cluster(v,c)||c/8U>=bytes)return FL32_RANGE;
 uint8_t mask=(uint8_t)(1U<<(c&7U));if(bits[c/8U]&mask)return FL32_CHAIN;
 bits[c/8U]|=mask;return FL32_OK;
}
static int own_file(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,uint8_t *bits,uint32_t bytes,uint32_t c,uint32_t size){
 if(!c)return size?FL32_FORMAT:FL32_OK;
 uint64_t minimum=((uint64_t)size+(uint64_t)v->spc*512U-1)/((uint64_t)v->spc*512U),count=0;
 for(;;){int r=own_cluster(bits,bytes,v,c);if(r)return r;count++;
  uint32_t next;r=fat_next(io,lim,w,v,c,&next);if(r)return r;
  if(next>=EOC)return count>=minimum?FL32_OK:FL32_CHAIN;
  c=next;
 }
}
static int own_tree(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,const struct volume *v,uint8_t *bits,uint32_t bytes,struct leo_log_dir *dirs,uint32_t cap){
 uint32_t used=1;dirs[0]=(struct leo_log_dir){v->root,0};
 for(uint32_t index=0;index<used;index++){
  uint32_t c=dirs[index].first;int ended=0;
  for(;;){int r=own_cluster(bits,bytes,v,c);if(r)return r;uint64_t lba;r=cluster_lba(v,c,&lba);if(r)return r;
   if(!ended)for(unsigned sector=0;sector<v->spc&&!ended;sector++){
    r=read_sector(io,lim,w,lba+sector,w->sector);if(r)return r;
    for(unsigned off=0;off<512;off+=32){const uint8_t *e=w->sector+off;
     if(!e[0]){ended=1;break;}if(e[0]==0xe5)continue;
     if(e[11]==0x0f){if(le16(e+26))return FL32_FORMAT;continue;}
     if(e[11]&0xc0)return FL32_FORMAT;
     uint32_t first=((uint32_t)le16(e+20)<<16)|le16(e+26),size=le32(e+28);
     if(e[11]&8){if(e[11]!=8||first||size)return FL32_FORMAT;continue;}
     if(e[0]=='.'){
      if(!memcmp(e,".          ",11)){if(!(e[11]&0x10)||size||first!=dirs[index].first)return FL32_FORMAT;continue;}
      if(!memcmp(e,"..         ",11)){if(!(e[11]&0x10)||size||(first!=dirs[index].parent&&!(dirs[index].parent==v->root&&first==0)))return FL32_FORMAT;continue;}
      return FL32_FORMAT;
     }
     if(e[11]&0x10){if(size||!data_cluster(v,first)||used>=cap)return FL32_LIMIT;dirs[used++]=(struct leo_log_dir){first,dirs[index].first};}
     else {r=own_file(io,lim,w,v,bits,bytes,first,size);if(r)return r;}
    }
   }
   uint32_t next;r=fat_next(io,lim,w,v,c,&next);if(r)return r;if(next>=EOC)break;c=next;
  }
 }
 return FL32_OK;
}
int leo_log_locate(const struct fl32_io *io,const struct fl32_limits *lim,struct fl32_work *w,struct fl32_extent *ext,uint32_t cap,struct fl32_image *out,const uint8_t nonce[16],uint8_t *owners,uint32_t owner_bytes,struct leo_log_dir *queue,uint32_t dir_cap){
 if(out)memset(out,0,sizeof(*out));
 if(!io||!lim||!w||!ext||!out||!nonce||!owners||!queue||!dir_cap||dir_cap>LEO_LOG_DIR_CAP||!io->read512||!io->sectors||!lim->read_budget||!cap||cap>128||!lim->directory_clusters||lim->directory_clusters>FL32_DIRECTORY_CAP)return FL32_ARGUMENT;
 memset(w,0,sizeof(*w));w->fat_lba[0]=w->fat_lba[1]=UINT64_MAX;struct volume v;memset(&v,0,sizeof(v));int r=volume_read(io,lim,w,&v);if(r)return r;
 if(v.clusters>LEO_LOG_CLUSTER_CAP||owner_bytes<((uint64_t)v.clusters+2U+7U)/8U)return FL32_LIMIT;
 static const uint8_t dirname[11]={'I','O','S','7',' ',' ',' ',' ',' ',' ',' '};
 static const uint8_t filename[11]={'B','O','O','T','L','O','G','8','B','I','N'};
 uint32_t dir=0,size=0,first=0;unsigned seen_dirs=0;
 r=find_name(io,lim,w,&v,v.root,dirname,1,&seen_dirs,&dir,&size);if(r)return r;
 r=find_name(io,lim,w,&v,dir,filename,0,&seen_dirs,&first,&size);if(r)return r;
 if(size!=LEO_LOG_BYTES)return FL32_FORMAT;
 struct fl32_image image;memset(&image,0,sizeof(image));image.file_bytes=size;image.image_sectors=size/512U;image.card_sectors=io->sectors;image.partition_lba=v.part;image.partition_sectors=v.total;image.data_lba=v.data;image.sectors_per_cluster=v.spc;image.first_cluster=first;
 r=image_chain(io,lim,w,&v,first,size,seen_dirs,ext,cap,&image.extent_count);if(r)return r;
 uint64_t physical;uint32_t run;r=translate_raw(&image,ext,cap,0,&physical,&run);if(r)return r;
 r=read_sector(io,lim,w,physical,w->primary);if(r)return r;
 if(leo_log_header_check(w->primary,nonce,size)!=LEO_LOG_OK)return FL32_FORMAT;
 memset(owners,0,owner_bytes);r=own_tree(io,lim,w,&v,owners,owner_bytes,queue,dir_cap);if(r)return r;
 image.magic=LEO_LOG_MAP_MAGIC;*out=image;return FL32_OK;
}
int leo_log_translate(const struct fl32_image *image,const struct fl32_extent *ext,uint32_t cap,uint64_t logical,uint64_t *physical,uint32_t *run){
 if(!image||image->magic!=LEO_LOG_MAP_MAGIC||image->file_bytes!=LEO_LOG_BYTES||image->image_sectors!=LEO_LOG_SECTORS)return FL32_ARGUMENT;
 return translate_raw(image,ext,cap,logical,physical,run);
}
