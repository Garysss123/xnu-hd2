#include "leo_log_format.h"
#include <string.h>
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t u64(const uint8_t *p){return u32(p)|((uint64_t)u32(p+4)<<32);}
static void w32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void w64(uint8_t *p,uint64_t n){w32(p,(uint32_t)n);w32(p+4,(uint32_t)(n>>32));}
static int nonzero(const uint8_t *p,unsigned n){unsigned v=0;for(unsigned i=0;i<n;i++)v|=p[i];return v!=0;}
uint32_t leo_log_crc32(const uint8_t *p,uint32_t n){uint32_t c=0xffffffffU;for(uint32_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^((0U-(c&1U))&0xedb88320U);}return ~c;}
int leo_log_header_check(const uint8_t h[512],const uint8_t nonce[16],uint64_t bytes){
 if(!h||!nonce||!nonzero(nonce,16))return LEO_LOG_ARGUMENT;
 if(bytes!=LEO_LOG_BYTES||memcmp(h,"LEOLOG01",8)||u32(h+8)!=1||u32(h+12)!=512||u64(h+16)!=bytes||u32(h+24)!=512||u32(h+28)!=2047||memcmp(h+32,nonce,16))return LEO_LOG_FORMAT;
 for(unsigned i=48;i<508;i++)if(h[i])return LEO_LOG_FORMAT;
 return u32(h+508)==leo_log_crc32(h,508)?LEO_LOG_OK:LEO_LOG_CRC;
}
int leo_log_record_make(uint8_t out[512],const uint8_t nonce[16],const uint8_t boot[16],uint64_t seq,uint64_t ns,uint32_t kind,uint32_t dropped,const uint8_t *p,uint32_t n){
 if(!out||!nonce||!boot||!nonzero(nonce,16)||!nonzero(boot,16)||!seq||seq>=LEO_LOG_SECTORS||(kind!=1&&kind!=2)||!n||n>LEO_LOG_PAYLOAD||!p)return LEO_LOG_ARGUMENT;
 memset(out,0,512);memcpy(out,"LEOREC01",8);w32(out+8,1);w32(out+12,kind);w64(out+16,seq);memcpy(out+24,boot,16);w64(out+40,ns);w32(out+48,n);w32(out+52,dropped);memcpy(out+56,nonce,16);memcpy(out+72,p,n);w32(out+508,leo_log_crc32(out,508));return LEO_LOG_OK;
}
int leo_log_record_check(const uint8_t r[512],const uint8_t nonce[16]){
 if(!r||!nonce)return LEO_LOG_ARGUMENT;
 uint32_t n=u32(r+48),kind=u32(r+12);uint64_t seq=u64(r+16);
 if(memcmp(r,"LEOREC01",8)||u32(r+8)!=1||(kind!=1&&kind!=2)||!seq||seq>=LEO_LOG_SECTORS||!n||n>LEO_LOG_PAYLOAD||memcmp(r+56,nonce,16)||!nonzero(r+24,16))return LEO_LOG_FORMAT;
 for(uint32_t i=72+n;i<508;i++)if(r[i])return LEO_LOG_FORMAT;
 return u32(r+508)==leo_log_crc32(r,508)?LEO_LOG_OK:LEO_LOG_CRC;
}
int leo_log_spool_init(leo_log_spool *s,uint8_t *bytes,uint32_t capacity){if(!s||!bytes||!capacity||capacity>65536)return LEO_LOG_ARGUMENT;*s=(leo_log_spool){bytes,capacity,0,0,0};return LEO_LOG_OK;}
void leo_log_spool_put(leo_log_spool *s,uint8_t c){
 if(!s||!s->bytes||!s->capacity||s->head>=s->capacity||s->count>s->capacity)return;
 if(s->count==s->capacity){if(s->dropped!=UINT32_MAX)s->dropped++;return;}
 s->bytes[(s->head+s->count)%s->capacity]=c;s->count++;
}
uint32_t leo_log_spool_snapshot(const leo_log_spool *s,uint8_t *out,uint32_t maximum){
 if(!s||!out||!s->bytes||!s->capacity||s->head>=s->capacity||s->count>s->capacity)return 0;
 uint32_t n=s->count<maximum?s->count:maximum;for(uint32_t i=0;i<n;i++)out[i]=s->bytes[(s->head+i)%s->capacity];return n;
}
int leo_log_spool_commit(leo_log_spool *s,uint32_t n){if(!s||!s->bytes||!s->capacity||s->head>=s->capacity||s->count>s->capacity||n>s->count)return LEO_LOG_ARGUMENT;s->head=(s->head+n)%s->capacity;s->count-=n;return LEO_LOG_OK;}
