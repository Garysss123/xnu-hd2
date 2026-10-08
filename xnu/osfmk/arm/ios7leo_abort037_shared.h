#ifndef IOS7LEO_ABORT037_SHARED_H
#define IOS7LEO_ABORT037_SHARED_H
#include <stdint.h>
#define IOS7LEO_ABORT037_RECORDS 7U
#define IOS7LEO_ABORT037_PAYLOAD_MAX 436U
#ifdef __cplusplus
extern "C" {
#endif
/* Existing logger worker only. One complete record is copied; no partial read.
 * Zero means no published record or insufficient caller capacity (not popped).
 * Maximum seven records once per boot; no timer, worker or device interface. */
uint32_t ios7leo_abort037_take(uint8_t *payload,uint32_t capacity);
#ifdef __cplusplus
}
#endif
#endif
