#include "leo_cow.h"
#include <string.h>
#define OCCUPIED 1U
#define DIRTY 2U
uint64_t leo_cow_total_pages(uint32_t capacity,uint32_t fixed_pages)
{
 uint32_t slots=1;
 if(capacity>UINT32_MAX/4096U)return UINT64_MAX;
 if(!capacity)return fixed_pages;
 while(slots<capacity*2U)slots<<=1;
 return (uint64_t)fixed_pages+capacity+(((uint64_t)slots*sizeof(leo_cow_entry)+4095U)>>12)+(((uint64_t)capacity*sizeof(uint32_t)+4095U)>>12);
}
uint32_t leo_cow_budget_pages(uint64_t managed,uint32_t free_pages,uint32_t reserved,uint32_t target,uint32_t fixed_pages)
{
 uint64_t guard=(managed>>2)+(uint64_t)reserved+target,cap,extra;uint32_t low=0,high;
 if((uint64_t)free_pages<=guard)return 0;
 extra=((uint64_t)free_pages-guard)>>1;cap=managed>>3;if(cap>extra)cap=extra;
 if(cap>UINT32_MAX/4096U)cap=UINT32_MAX/4096U;
 high=(uint32_t)cap;
 while(low<high){uint32_t mid=low+((high-low+1U)>>1);if(leo_cow_total_pages(mid,fixed_pages)<=cap)low=mid;else high=mid-1U;}
 return low;
}
static leo_cow_entry *lookup(leo_cow *c,uint32_t page,int empty)
{
 uint32_t i,slot=(page*2654435761U)&(c->hash_slots-1U);
 for(i=0;i<c->hash_slots;i++,slot=(slot+1U)&(c->hash_slots-1U)) {
  leo_cow_entry *e=c->entries+slot;
  if(!(e->flags&OCCUPIED))return empty?e:0;
  if(e->page==page)return e;
 }
 return 0;
}
int leo_cow_init(leo_cow *c,uint64_t sectors,uint32_t capacity,uint32_t slots,uint8_t *arena,leo_cow_entry *entries,uint32_t *pending,void *cookie,leo_cow_read512_fn reader)
{
 if(!c||!arena||!entries||!pending||!reader||!sectors||(sectors&7U)||
    (sectors>>3)>UINT32_MAX||!capacity||capacity>UINT32_MAX/4096U||capacity>(sectors>>3)||
    !slots||(slots&(slots-1U))||slots<capacity*2U||slots>UINT32_MAX/sizeof(*entries))return LEO_COW_RANGE;
 memset(c,0,sizeof(*c));memset(entries,0,slots*sizeof(*entries));
 c->sectors=sectors;c->capacity=capacity;c->hash_slots=slots;c->arena=arena;c->entries=entries;c->pending=pending;c->cookie=cookie;c->read512=reader;
 return LEO_COW_OK;
}
int leo_cow_read_sector(leo_cow *c,uint64_t sector,uint8_t out[512])
{
 leo_cow_entry *e;int error;
 if(!c||!out||sector>=c->sectors)return LEO_COW_RANGE;
 e=lookup(c,(uint32_t)(sector>>3),0);
 if(e){memcpy(out,c->arena+(e->slot<<12)+((uint32_t)(sector&7U)<<9),512);return LEO_COW_OK;}
 error=c->read512(c->cookie,sector,out);if(error){c->backend_error=error;return LEO_COW_IO;}return LEO_COW_OK;
}
int leo_cow_prepare_write(leo_cow *c,uint64_t first,uint64_t count)
{
 uint32_t first_page,last_page,page,needed=0,i,j;
 if(!c||!count||first>=c->sectors||count>c->sectors-first)return LEO_COW_RANGE;
 first_page=(uint32_t)(first>>3);last_page=(uint32_t)((first+count-1U)>>3);
 if(last_page-first_page+1U>c->capacity)return LEO_COW_NO_SPACE;
 for(page=first_page;;page++){
  if(!lookup(c,page,0)){
   if(needed>=c->capacity-c->used)return LEO_COW_NO_SPACE;
   c->pending[needed++]=page;
  }
  if(page==last_page)break;
 }
 /* Capacity is proved before reads. Pending fills are not visible until all
  * original pages have been read; no payload allocation occurs under I/O lock. */
 for(i=0;i<needed;i++)for(j=0;j<8U;j++){
  int error=c->read512(c->cookie,((uint64_t)c->pending[i]<<3)+j,c->arena+((c->used+i)<<12)+(j<<9));
  if(error){c->backend_error=error;return LEO_COW_IO;}
 }
 for(i=0;i<needed;i++){
  leo_cow_entry *e=lookup(c,c->pending[i],1);if(!e)return LEO_COW_STATE;
  e->page=c->pending[i];e->slot=c->used+i;e->flags=OCCUPIED;
 }
 c->used+=needed;return LEO_COW_OK;
}
int leo_cow_write_prepared_sector(leo_cow *c,uint64_t sector,const uint8_t bytes[512])
{
 leo_cow_entry *e;
 if(!c||!bytes||sector>=c->sectors)return LEO_COW_RANGE;
 e=lookup(c,(uint32_t)(sector>>3),0);if(!e)return LEO_COW_STATE;
 memcpy(c->arena+(e->slot<<12)+((uint32_t)(sector&7U)<<9),bytes,512);
 if(!(e->flags&DIRTY)){e->flags|=DIRTY;c->dirty++;}return LEO_COW_OK;
}
