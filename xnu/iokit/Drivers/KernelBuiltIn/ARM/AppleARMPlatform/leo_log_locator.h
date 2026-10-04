#ifndef LEO_OWNED_LOG_LOCATOR_H
#define LEO_OWNED_LOG_LOCATOR_H
#include "fat_locator.h"
#include "leo_log_format.h"
#define LEO_LOG_MAP_MAGIC 0x4c4f4731U
#define LEO_LOG_CLUSTER_CAP 2097152U
#define LEO_LOG_DIR_CAP 4096U
struct leo_log_dir {uint32_t first,parent;};
/* Caller heap memory, not stack: bitmap for all volume clusters and a bounded
 * directory queue. All allocated chains are marked, including after END.
 * Duplicate ownership anywhere rejects authorization. No payload walk/write. */
int leo_log_locate(const struct fl32_io *,const struct fl32_limits *,struct fl32_work *,struct fl32_extent *,uint32_t,struct fl32_image *,const uint8_t nonce[16],uint8_t *owners,uint32_t owner_bytes,struct leo_log_dir *,uint32_t dir_cap);
int leo_log_translate(const struct fl32_image *,const struct fl32_extent *,uint32_t,uint64_t,uint64_t *,uint32_t *);
#endif
