#ifndef IOS7LEO_TOUCH_TRANSPORT_H
#define IOS7LEO_TOUCH_TRANSPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Caller owns four aligned 4KiB native mappings and serializes calls on its
 * normal workloop. GPIO95/96 mux is inherited from the known MAGLDR/WinCE boot;
 * this transport does not touch ProcComm, modem mailboxes, IRQs or buttons. */
enum {
 LEO_TOUCH_I2C=0, LEO_TOUCH_CLOCK=1, LEO_TOUCH_GPIO1=2, LEO_TOUCH_GPIO2=3,
 LEO_TOUCH_REGIONS=4
};
enum {
 LEO_TOUCH_OK=0, LEO_TOUCH_ARGUMENT=-1, LEO_TOUCH_CLOCK_ERROR=-2,
 LEO_TOUCH_TIMEOUT=-3, LEO_TOUCH_BUS_ERROR=-4, LEO_TOUCH_PROTOCOL=-5,
 LEO_TOUCH_GPIO_ERROR=-6, LEO_TOUCH_NOT_READY=-7
};
enum {
 LEO_TOUCH_STAGE_CONFIG=1, LEO_TOUCH_STAGE_RESET=2, LEO_TOUCH_STAGE_CLOCK=3,
 LEO_TOUCH_STAGE_ID=4, LEO_TOUCH_STAGE_ENABLE=5, LEO_TOUCH_STAGE_READY=6,
 LEO_TOUCH_STAGE_READ=7, LEO_TOUCH_STAGE_REPORT=8
};
typedef struct LeoTouchIO {
 void *cookie;
 uint32_t (*read)(void *,uint32_t region,uint32_t offset);
 void (*write)(void *,uint32_t region,uint32_t offset,uint32_t value);
 uint64_t (*now_ns)(void *);
 int (*delay_us)(void *,uint32_t microseconds);
 uint32_t poll_bound;
} LeoTouchIO;
typedef struct LeoTouchReport {
 uint32_t sequence;
 uint16_t x,y; /* Calibrated physical portrait pixels: 0..479 / 0..799. */
 uint8_t down; /* 0 or 1; zero comes only from a valid no-contact packet. */
 uint8_t contacts; /* Actual hardware count 0..2. Only its first contact is used. */
 uint8_t raw[9];
} LeoTouchReport;
typedef struct LeoTouchState {
 LeoTouchIO io;
 uint32_t ready,stage;
 int32_t last_result;
 uint32_t last_diagnostic,last_status,id_word;
 uint32_t init_cycles,polls,not_ready_skips,reads,packets,errors,timeouts;
 uint32_t recoveries,high_polls,presses,releases,last_contacts,sequence;
 uint16_t last_x,last_y;
 uint8_t last_raw[9];
 uint64_t transfer_start_ns;
} LeoTouchState;

/* init: zero on actual Type2A ID + enable success, negative status otherwise.
 * poll: 1 for a complete real report, 0 for no data, negative actual error.
 * On poll errors the caller's report and last successful contact stay intact.
 * There is no mapping allocation or cleanup here; the caller owns mappings. */
int leo_touch_transport_init(LeoTouchState *,const LeoTouchIO *);
int leo_touch_transport_poll(LeoTouchState *,LeoTouchReport *);
#ifdef __cplusplus
}
#endif
#endif
