#ifndef IOS7_LEO_COW_H
#define IOS7_LEO_COW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LEO_COW_SECTOR_BYTES 512U
#define LEO_COW_PAGE_BYTES 4096U
enum leo_cow_result { LEO_COW_OK=0, LEO_COW_RANGE=1, LEO_COW_NO_SPACE=2, LEO_COW_IO=3, LEO_COW_STATE=4 };
typedef int (*leo_cow_read512_fn)(void *,uint64_t,uint8_t[512]);
typedef struct { uint32_t page,slot,flags; } leo_cow_entry;
typedef struct {
 uint64_t sectors;uint32_t capacity,used,dirty,hash_slots;
 uint8_t *arena;leo_cow_entry *entries;uint32_t *pending;
 void *cookie;leo_cow_read512_fn read512;int backend_error;
} leo_cow;
uint32_t leo_cow_budget_pages(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t);
uint64_t leo_cow_total_pages(uint32_t,uint32_t);
int leo_cow_init(leo_cow *,uint64_t,uint32_t,uint32_t,uint8_t *,leo_cow_entry *,uint32_t *,void *,leo_cow_read512_fn);
int leo_cow_read_sector(leo_cow *,uint64_t,uint8_t[512]);
int leo_cow_prepare_write(leo_cow *,uint64_t,uint64_t);
int leo_cow_write_prepared_sector(leo_cow *,uint64_t,const uint8_t[512]);
#ifdef __cplusplus
}
#endif
#endif
