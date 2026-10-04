#ifndef IOS7LEO_SDCC_H
#define IOS7LEO_SDCC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum leo_sd_region { LEO_SDCC, LEO_PROC, LEO_CSR, LEO_CLOCK };
enum leo_sd_result { LEO_SD_OK=0, LEO_SD_ARGUMENT=-1, LEO_SD_TIMEOUT=-2,
 LEO_SD_RPC_BUSY=-3, LEO_SD_RPC_FAILED=-4, LEO_SD_RPC_UNCERTAIN=-5,
 LEO_SD_COMMAND=-6, LEO_SD_DATA=-7, LEO_SD_PROTOCOL=-8, LEO_SD_SHORT=-9,
 LEO_SD_FAULTED=-10, LEO_SD_CLOCK=-11, LEO_SD_WRITE_PROTECTED=-12,
 LEO_SD_NO_MEMORY=-13 };
/* Callbacks are serialized by the owner. Every native read/write callback uses
 * architectural device ordering (DSB), for all four regions. now_ns is genuine monotonic time;
 * delay_us must wait at least the requested interval and return on clock fault.
 * poll_bound additionally prevents a stalled timer from creating an infinite loop. */
struct leo_sd_io {
 void *cookie;
 uint32_t (*read)(void *,unsigned,unsigned);
 void (*write)(void *,unsigned,unsigned,uint32_t);
 uint64_t (*now_ns)(void *);
 int (*delay_us)(void *,uint32_t);
 uint32_t poll_bound;
};
struct leo_sd_card {
 struct leo_sd_io io;
 uint64_t sectors;
 uint32_t cid[4],csd[4],rca,ocr;
 uint32_t stage,command,status,rpc_status,last_rpc_command,last_rpc_arg1,last_rpc_arg2;
 uint32_t requested_clock_hz,requested_selector,csd_transfer_hz;
 int slow_read0_ok,transfer_clock_attempted,fast_read0_verified;
 int high_capacity,csd_write_protected,initialized,faulted,rpc_uncertain;
 uint32_t write_attempts,write_words,write_completed;
 int write_result;
};
void leo_sd_init(struct leo_sd_card *,const struct leo_sd_io *);
int leo_sd_start(struct leo_sd_card *);
int leo_sd_read_sector(struct leo_sd_card *,uint64_t,uint8_t [512]);
/* Private owned-log capability caller only; never the ordinary raw-media path.
 * Exactly one512B CMD24, DATAEND+count0+emptyFIFO and actual CMD13 ready/TRAN.
 * Any issued fault remains latched; no reset/automatic retry or writeback claim. */
int leo_sd_write_sector(struct leo_sd_card *,uint64_t,const uint8_t [512]);
/* Cached CSD maximum:0 means unusable/reserved advertisement, not measured Hz. */
uint32_t leo_sd_csd_transfer_rate(const uint32_t [4]);
/* Startup only, owner mutex held. Actual successful slow read0 must precede this
 * single attempt. Unsupported advertisement stays slow; all issued errors latch.
 * Successful fast request requires CMD13 + full read0 matching slow_reference. */
int leo_sd_transfer_clock(struct leo_sd_card *,const uint8_t slow_reference[512]);
/* Exposed for the same portable host fixture and single-owner native startup. */
int leo_sd_rpc(struct leo_sd_card *,uint32_t,uint32_t *,uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
