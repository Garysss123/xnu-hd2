#ifndef IOS7_LEO_DISPLAY_BACKEND_H
#define IOS7_LEO_DISPLAY_BACKEND_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Genuine mapped PE backend, no arbitrary physical address argument. */
int ios7leo_display_ready(void);
int ios7leo_display_present(const uint8_t *pixels,uint32_t bytes,uint32_t row,
                           uint32_t format,uint64_t *completed_sequence);
int ios7leo_display_completed(uint64_t sequence);
#ifdef __cplusplus
}
#endif
#endif
