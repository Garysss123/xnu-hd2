#ifndef IOS7LEO_PRIVATE_SD_LOG_H
#define IOS7LEO_PRIVATE_SD_LOG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private kernel-only interface. Logger must retain/open its raw IOMedia as a
 * reader and validate exact IOS7/BOOTLOG.BIN header/FAT chain, including root,
 * directory and metadata disjointness, before bind. No user ioctl or raw LBA API.
 * Service is the actual IOS7LeoSDCC2 ancestor of that retained raw IOMedia. */
#define IOS7LEO_SD_LOG_FILE_SECTORS 2048U
#define IOS7LEO_SD_LOG_MAX_EXTENTS 128U
struct ios7leo_sd_log_extent {
 uint64_t file_sector,card_sector;
 uint32_t sectors;
};
struct ios7leo_sd_log_info {
 uint64_t card_sectors;
 uint32_t cid[4],csd_write_protected,initialized,faulted;
 uint32_t stage,command,status,write_attempts,write_words,write_completed;
 int32_t write_result;
 uint32_t first_write_verified,next_file_sector;
};
typedef struct ios7leo_sd_log_capability ios7leo_sd_log_capability;
int ios7leo_sd_log_describe(void *service,struct ios7leo_sd_log_info *);
/* Entire logical file [0,2048) must be mapped once, contiguously in file order,
 * without physical extent overlap. Driver freezes a private copy and retains
 * service for boot lifetime. nonce is the logger-validated owned header nonce.
 * Bind only once; no hotplug/reset/rebind/authorization-after-timeout. */
int ios7leo_sd_log_bind(void *service,const struct ios7leo_sd_log_extent *,
 uint32_t extent_count,uint32_t first_writable_sector,const uint8_t nonce[16],
 ios7leo_sd_log_capability **);
/* Synchronous normal-worker context only, with no console/IRQ/VM locks held.
 * Writes only the exact next logical sector 1..2047; header0 is NEVER written.
 * Logger scans every old slot and sets first_writable beyond ALL nonzero slots,
 * including torn records, so old data is never overwritten/retried. 2048=full.
 * Per-request exact512B transfer + actual ready-for-data/TRAN completion. First
 * successful write is reread and compared before success; no blind retry after
 * issued failure, no claim of power-loss atomicity or SD cache persistence.
 * Ordinary raw IOBlockStorageDevice remains read-only/write-protected. */
int ios7leo_sd_log_write(ios7leo_sd_log_capability *,uint32_t file_sector,
 const uint8_t bytes[512]);
#ifdef __cplusplus
}
#endif
#endif
