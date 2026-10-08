#ifndef IOS7_LEO_APPFAULT057_SHARED_H
#define IOS7_LEO_APPFAULT057_SHARED_H
#include <stdint.h>
#define LEO_APPFAULT057_SLOTS 8U
#define LEO_APPFAULT057_RECORDS 11U
#define LEO_APPFAULT057_PAYLOAD 436U
#ifdef __cplusplus
extern "C" {
#endif
/* Existing worker only; at most89 complete records per boot, no SD in trap. */
uint32_t ios7leo_appfault057_take(uint8_t *payload,uint32_t capacity);
#ifdef __cplusplus
}
#endif
#endif
