#ifndef LEO_DEMAND_READ_CACHE_H
#define LEO_DEMAND_READ_CACHE_H
#include <stdint.h>
#include <string.h>
#define LEO_READ_CACHE_SLOTS 64U
typedef int (*leo_read_cache_read_fn)(void *,uint64_t,uint8_t[512]);
typedef int (*leo_read_cache_media_fn)(void *);
typedef struct {uint64_t sector,age;uint8_t bytes[512];} leo_read_cache_entry;
typedef struct {
 uint64_t sectors,clock,hits,misses,base_reads;
 leo_read_cache_entry entry[LEO_READ_CACHE_SLOTS];
} leo_read_cache;
static inline void leo_read_cache_init(leo_read_cache *c,uint64_t sectors)
{memset(c,0,sizeof(*c));c->sectors=sectors;}
/* Caller serializes under the existing root-file mutex. The raw immutable
 * base and COW overlay are separate: caller checks overlay before this hook.
 * One miss issues exactly one original demand read, never prefetch. */
static inline int leo_read_cache_read(leo_read_cache *c,uint64_t sector,uint8_t out[512],
 void *cookie,leo_read_cache_read_fn reader,leo_read_cache_media_fn media,int range_error)
{
 uint32_t i,victim=0;int error;
 if(!c||!out||!reader||!media||!c->sectors||sector>=c->sectors)return range_error;
 for(i=0;i<LEO_READ_CACHE_SLOTS;i++){
  leo_read_cache_entry *e=c->entry+i;
  if(e->age&&e->sector==sector){
   error=media(cookie);if(error)return error;
   memcpy(out,e->bytes,512);c->hits++;
   if(c->clock==UINT64_MAX){for(uint32_t j=0;j<LEO_READ_CACHE_SLOTS;j++)c->entry[j].age=0;c->clock=0;}
   e->age=++c->clock;return 0;
  }
 }
 c->misses++;c->base_reads++;error=reader(cookie,sector,out);
 if(error)return error; /* Failed/short backend reads never become valid. */
 for(i=0;i<LEO_READ_CACHE_SLOTS;i++){
  if(!c->entry[i].age){victim=i;break;}
  if(c->entry[i].age<c->entry[victim].age)victim=i;
 }
 if(c->clock==UINT64_MAX){for(i=0;i<LEO_READ_CACHE_SLOTS;i++)c->entry[i].age=0;c->clock=0;victim=0;}
 c->entry[victim].sector=sector;memcpy(c->entry[victim].bytes,out,512);
 c->entry[victim].age=++c->clock;return 0;
}
#endif
