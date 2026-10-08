#ifndef IOS7_LEO_SCANOUT_ROW_H
#define IOS7_LEO_SCANOUT_ROW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One validated320-pixel input row to480 physical output pixels. The caller owns IRQ/fatal exclusion. */
void leo_scanout_copy_row(volatile uint16_t *out,const uint8_t *pixels,uint32_t format);
#ifdef __cplusplus
}
#endif
#endif
