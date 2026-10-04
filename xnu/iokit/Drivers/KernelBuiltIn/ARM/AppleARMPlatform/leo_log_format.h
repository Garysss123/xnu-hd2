#ifndef LEO_OWNED_LOG_FORMAT_H
#define LEO_OWNED_LOG_FORMAT_H
#include <stdint.h>
#define LEO_LOG_BYTES 1048576U
#define LEO_LOG_SECTORS 2048U
#define LEO_LOG_PAYLOAD 436U
#define LEO_LOG_SPOOL_BYTES 16384U
enum leo_log_error { LEO_LOG_OK=0,LEO_LOG_ARGUMENT=1,LEO_LOG_FORMAT=2,LEO_LOG_CRC=3,LEO_LOG_FULL=4 };
typedef struct {uint8_t *bytes;uint32_t capacity,head,count,dropped;} leo_log_spool;
uint32_t leo_log_crc32(const uint8_t *,uint32_t);
int leo_log_header_check(const uint8_t[512],const uint8_t nonce[16],uint64_t file_bytes);
int leo_log_record_make(uint8_t[512],const uint8_t nonce[16],const uint8_t boot_id[16],uint64_t sequence,uint64_t actual_ns,uint32_t kind,uint32_t dropped,const uint8_t *,uint32_t);
int leo_log_record_check(const uint8_t[512],const uint8_t nonce[16]);
/* Caller serializes producer/snapshot/commit using its tiny ring lock.
 * These functions do no allocation, scheduling or I/O. Commit only after the
 * normal worker's actual successful512B private transport completion. */
int leo_log_spool_init(leo_log_spool *,uint8_t *,uint32_t);
void leo_log_spool_put(leo_log_spool *,uint8_t);
uint32_t leo_log_spool_snapshot(const leo_log_spool *,uint8_t *,uint32_t);
int leo_log_spool_commit(leo_log_spool *,uint32_t);
#endif
