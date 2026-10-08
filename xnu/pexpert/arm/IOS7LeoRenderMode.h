#ifndef IOS7_LEO_RENDER_MODE_H
#define IOS7_LEO_RENDER_MODE_H
#include <stdint.h>
/* Virtual CPU canvas only. Physical boot/MDP/console and normalized HID remain
 * 480x800. 533 integer lines approximate a 1.5x scale (vertical error 0.063%). */
enum { LEO_RENDER_WIDTH=320, LEO_RENDER_HEIGHT=533, LEO_RENDER_ROW=1280,
       LEO_RENDER_ACTIVE_BYTES=682240, LEO_RENDER_ALLOC_BYTES=684032,
       LEO_PANEL_WIDTH=480, LEO_PANEL_HEIGHT=800 };
/* Called only with a validated physical row in [0,800). */
static inline uint32_t leo_render_source_y(uint32_t y){return y*533U/800U;}
#endif
