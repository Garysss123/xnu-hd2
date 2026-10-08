#ifndef LEO_SCANNED_CPU_FRAME_H
#define LEO_SCANNED_CPU_FRAME_H
#include <stdint.h>
#include "IOS7LeoScanoutRow.h"
#include "IOS7LeoRenderMode.h"
static int leo_scanout_geometry(uint32_t bytes,uint32_t row,uint32_t format)
{
 uint64_t active=(uint64_t)(LEO_RENDER_HEIGHT-1U)*row+LEO_RENDER_WIDTH*4U;
 return (format==0x42475241U||format==0x52474241U)&&row>=LEO_RENDER_ROW&&row<=16384U&&!(row&3U)&&active<=bytes&&bytes<=16U*1024U*1024U;
}
#endif
