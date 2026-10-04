#ifndef LEO_SCANNED_CPU_FRAME_H
#define LEO_SCANNED_CPU_FRAME_H
#include <stdint.h>
static int leo_scanout_geometry(uint32_t bytes,uint32_t row,uint32_t format)
{
 uint64_t active=(uint64_t)799U*row+480U*4U;
 return (format==0x42475241U||format==0x52474241U)&&row>=1920U&&row<=16384U&&!(row&3U)&&active<=bytes&&bytes<=16U*1024U*1024U;
}
static uint16_t leo_scanout_rgb565(const uint8_t *p,uint32_t format)
{
 uint8_t r=format==0x42475241U?p[2]:p[0],g=p[1],b=format==0x42475241U?p[0]:p[2];
 return (uint16_t)(((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3));
}
static void leo_scanout_copy_row(volatile uint16_t *out,const uint8_t *pixels,uint32_t format)
{for(unsigned int x=0;x<480;x++)out[x]=leo_scanout_rgb565(pixels+x*4,format);}
#endif
