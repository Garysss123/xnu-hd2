/* Owned QSD8250 Leo PIO PAL; ordinary media remains read-only. Numeric facts crosschecked
 * against local HTC Linux SDCC/WinCE clock and read-only Leo EKA2 BSP sources.
 * Private CMD24 is only for the validated owned-log capability. No Linux/EKA2
 * ABI, DMA, IRQ callback or private VA reused. */
#include "leo_sdcc.h"
#include "IOS7LabSDProtocol.h"
#define BIT(n) (1U<<(n))
#define STATUS 0x34U
#define CLEAR 0x38U
#define CMDERR (BIT(0)|BIT(2))
#define DATAERR (BIT(1)|BIT(3)|BIT(4)|BIT(5))
#define R1ERR 0xfdffe008U
#define SECOND 1000000000ULL
static uint32_t rd(struct leo_sd_card *c,unsigned r,unsigned o)
{ return c->io.read(c->io.cookie,r,o); }
static void wr(struct leo_sd_card *c,unsigned r,unsigned o,uint32_t v)
{ c->io.write(c->io.cookie,r,o,v); }
static int delay(struct leo_sd_card *c,uint32_t us)
{ return c->io.delay_us(c->io.cookie,us)?LEO_SD_OK:LEO_SD_CLOCK; }
static int expired(struct leo_sd_card *c,uint64_t start,uint64_t limit,uint32_t n)
{ return n>=c->io.poll_bound || c->io.now_ns(c->io.cookie)-start>=limit; }
static int regwrite(struct leo_sd_card *c,unsigned off,uint32_t value)
{ wr(c,LEO_SDCC,off,value);return delay(c,50U); }
void leo_sd_init(struct leo_sd_card *c,const struct leo_sd_io *io)
{
 unsigned i;uint8_t *p=(uint8_t *)c;
 for(i=0;i<sizeof(*c);++i)p[i]=0;
 c->io=*io;
 c->command=c->status=c->rpc_status=UINT32_MAX;
 c->last_rpc_command=c->last_rpc_arg1=c->last_rpc_arg2=UINT32_MAX;
}
int leo_sd_rpc(struct leo_sd_card *c,uint32_t cmd,uint32_t *a,uint32_t *b)
{
 uint64_t begin;uint32_t n=0,status;
 if(!a||!b||!cmd)return LEO_SD_ARGUMENT;
 c->last_rpc_command=cmd;c->last_rpc_arg1=*a;c->last_rpc_arg2=*b;
 c->rpc_status=UINT32_MAX; /* A previous reply does not belong to this attempt. */
 if(c->rpc_uncertain)return LEO_SD_RPC_UNCERTAIN;
 /* Shared mailbox ownership is not stolen from another firmware client. */
 if(rd(c,LEO_PROC,0)!=0)return LEO_SD_RPC_BUSY;
 begin=c->io.now_ns(c->io.cookie);
 while(rd(c,LEO_PROC,0x14)!=1U) {
  if(expired(c,begin,SECOND,++n))return LEO_SD_TIMEOUT;
 }
 if(rd(c,LEO_PROC,0)!=0)return LEO_SD_RPC_BUSY;
 wr(c,LEO_PROC,0,cmd);wr(c,LEO_PROC,8,*a);wr(c,LEO_PROC,12,*b);
 /* Native write callback performs DSB; doorbell is sent after mailbox stores. */
 wr(c,LEO_CSR,0x418,1U);begin=c->io.now_ns(c->io.cookie);n=0;
 while(rd(c,LEO_PROC,0)!=1U) {
  if(expired(c,begin,SECOND,++n)) {
   c->rpc_uncertain=1;return LEO_SD_RPC_UNCERTAIN;
  }
 }
 status=rd(c,LEO_PROC,4);c->rpc_status=status;
 if(status==3U){*a=rd(c,LEO_PROC,8);*b=rd(c,LEO_PROC,12);}
 /* DONE owns a completed reply even on failure: restore IDLE on both paths. */
 wr(c,LEO_PROC,0,0U);
 return status==3U?LEO_SD_OK:LEO_SD_RPC_FAILED;
}
static int rpc_values(struct leo_sd_card *c,uint32_t cmd,uint32_t a,uint32_t b)
{ return leo_sd_rpc(c,cmd,&a,&b); }
static int power_clock(struct leo_sd_card *c)
{
 unsigned pin;int r;uint32_t value;
 c->stage=1;
 r=rpc_values(c,30U,23U,1U);if(r)return r; /* GP6 enable */
 for(pin=62U;pin<=67U;++pin) {
  value=(pin<<4)|1U|BIT(14)|((pin==62U?0U:3U)<<15)|((pin<=63U?3U:1U)<<17);
  r=rpc_values(c,37U,value,0U);if(r)return r;
 }
 r=rpc_values(c,31U,23U,2850U);if(r)return r;
 c->stage=2;
 r=rpc_values(c,50U,16U,0U);if(r)return r; /* SDCC2 PCLK */
 r=rpc_values(c,58U,67U,1U);if(r)return r; /* Leo firmware 144kHz selector */
 r=rpc_values(c,50U,67U,0U);if(r)return r;
 c->requested_clock_hz=144000U;c->requested_selector=1U;
 value=rd(c,LEO_CLOCK,0);wr(c,LEO_CLOCK,0,value|BIT(8));
 if(!(rd(c,LEO_CLOCK,0)&BIT(8)))return LEO_SD_CLOCK;
 r=regwrite(c,0x3c,0);if(r)return r;
 r=regwrite(c,0x40,0);if(r)return r;
 /* Own no inherited transfer. Stop both state machines with the actual
  * post-write delay, then reject retained activity/FIFO instead of flushing. */
 r=regwrite(c,0x0c,0);if(r)return r;
 r=regwrite(c,0x2c,0);if(r)return r;
 value=rd(c,LEO_SDCC,STATUS);c->status=value;
 if(value&(BIT(11)|BIT(12)|BIT(13)|BIT(21)))return LEO_SD_DATA;
 r=regwrite(c,CLEAR,0x07c007ffU);if(r)return r;
 r=regwrite(c,4,BIT(8)|BIT(12)|BIT(15));if(r)return r; /* One-bit */
 r=regwrite(c,0,2);if(r)return r;
 r=delay(c,1000);if(r)return r;
 r=regwrite(c,0,3);if(r)return r;
 return delay(c,1000);
}
/* response:0 none,1 R1/R6/R7,2 R2,3 R3; busy only for R1b. */
static int command(struct leo_sd_card *c,uint32_t cmd,uint32_t arg,
                   unsigned response,int busy,int data,uint32_t out[4])
{
 uint32_t flags=cmd|BIT(10),status,n=0;uint64_t begin;unsigned i;int r;
 c->command=cmd;
 r=regwrite(c,0x0c,0);if(r)return r;
 r=regwrite(c,CLEAR,CMDERR|BIT(6)|BIT(7)|BIT(23));if(r)return r;
 if(response)flags|=BIT(6);
 if(response==2U)flags|=BIT(7);
 if(busy)flags|=BIT(11);
 if(data)flags|=BIT(12);
 wr(c,LEO_SDCC,8,arg);wr(c,LEO_SDCC,0x0c,flags);
 begin=c->io.now_ns(c->io.cookie);
 for(;;) {
  status=rd(c,LEO_SDCC,STATUS);c->status=status;
  if(status&BIT(2))return LEO_SD_TIMEOUT;
  if((status&BIT(0))&&response!=3U)return LEO_SD_COMMAND;
  if(status&(response?(BIT(6)|(response==3U?BIT(0):0U)):BIT(7)))break;
  if(expired(c,begin,SECOND,++n))return LEO_SD_TIMEOUT;
 }
 if(out&&response)for(i=0;i<4U;++i)out[i]=rd(c,LEO_SDCC,0x14U+4U*i);
 if(busy) {
  begin=c->io.now_ns(c->io.cookie);n=0;
  for(;;) {
   status=rd(c,LEO_SDCC,STATUS);c->status=status;
   if(status&BIT(2))return LEO_SD_TIMEOUT;
   if(status&(BIT(0)|DATAERR))return LEO_SD_COMMAND;
   if(status&BIT(23))break;
   if(expired(c,begin,SECOND,++n))return LEO_SD_TIMEOUT;
  }
 }
 return LEO_SD_OK;
}
static int ready(struct leo_sd_card *c)
{
 uint32_t resp[4];unsigned attempt;int r;
 for(attempt=0;attempt<100U;++attempt) {
  r=command(c,13U,c->rca,1U,0,0,resp);if(r)return r;
  if(resp[0]&R1ERR)return LEO_SD_PROTOCOL;
  if((resp[0]&BIT(8))&&((resp[0]>>9)&15U)==4U)return LEO_SD_OK;
  r=delay(c,1000);if(r)return r;
 }
 return LEO_SD_TIMEOUT;
}
int leo_sd_start(struct leo_sd_card *c)
{
 uint32_t resp[4];unsigned i,attempt;int r,v2,protected_media=0,done=0;
 if(!c||!c->io.read||!c->io.write||!c->io.now_ns||!c->io.delay_us||!c->io.poll_bound)return LEO_SD_ARGUMENT;
 if(c->faulted)return LEO_SD_FAULTED;
 r=power_clock(c);if(r)goto fail;
 c->stage=3;
 r=command(c,0,0,0,0,0,0);if(r)goto fail;
 r=command(c,8,0x1aaU,1,0,0,resp);v2=r==LEO_SD_OK;
 if(v2&&resp[0]!=0x1aaU){r=LEO_SD_PROTOCOL;goto fail;}
 if(!v2&&r!=LEO_SD_TIMEOUT)goto fail; /* Only expected SDv1 no-response */
 for(attempt=0;attempt<100U;++attempt) {
  r=command(c,55,0,1,0,0,resp);if(r)goto fail;
  if((resp[0]&R1ERR)||!(resp[0]&BIT(5))){r=LEO_SD_PROTOCOL;goto fail;}
  r=command(c,41,0x00010000U|(v2?BIT(30):0U),3,0,0,resp);if(r)goto fail;
  if(resp[0]&BIT(31)) {done=1;c->ocr=resp[0];c->high_capacity=!!(resp[0]&BIT(30));break;}
  r=delay(c,10000);if(r)goto fail;
 }
 if(!done){r=LEO_SD_TIMEOUT;goto fail;}
 if(!(c->ocr&0x00010000U)||(!v2&&c->high_capacity)){r=LEO_SD_PROTOCOL;goto fail;}
 c->stage=4;
 r=command(c,2,0,2,0,0,resp);if(r)goto fail;
 for(i=0;i<4U;++i)c->cid[i]=resp[i];
 r=command(c,3,0,1,0,0,resp);if(r)goto fail;
 if((resp[0]&0xe000U)||!(resp[0]&0xffff0000U)){r=LEO_SD_PROTOCOL;goto fail;}
 c->rca=resp[0]&0xffff0000U;
 r=command(c,9,c->rca,2,0,0,resp);if(r)goto fail;
 for(i=0;i<4U;++i)c->csd[i]=resp[i];
 if(!ios7lab_sd_capacity(c->csd,c->high_capacity,&c->sectors,&protected_media)){r=LEO_SD_PROTOCOL;goto fail;}
 c->csd_write_protected=protected_media;
 c->stage=5;
 r=command(c,7,c->rca,1,1,0,resp);if(r)goto fail;
 if(resp[0]&R1ERR){r=LEO_SD_PROTOCOL;goto fail;}
 if(!c->high_capacity){r=command(c,16,512,1,0,0,resp);if(r)goto fail;if(resp[0]&R1ERR){r=LEO_SD_PROTOCOL;goto fail;}}
 r=ready(c);if(r)goto fail;
 c->initialized=1;return LEO_SD_OK;
fail:
 c->faulted=1;return r;
}
int leo_sd_read_sector(struct leo_sd_card *c,uint64_t sector,uint8_t bytes[512])
{
 uint32_t arg,resp[4],status,word,n=0,words=0;uint64_t begin;unsigned i;int r;
 if(!c||!bytes||sector>=c->sectors||!ios7lab_sd_command_address(sector,c->high_capacity,&arg))return LEO_SD_ARGUMENT;
 if(!c->initialized||c->faulted)return LEO_SD_FAULTED;
 c->stage=6;
 r=regwrite(c,0x2c,0);if(r)goto fail;
 status=rd(c,LEO_SDCC,STATUS);c->status=status;
 /* W1C flags do not drain a FIFO. Never consume inherited/stale data. */
 if(status&(BIT(21)|BIT(12)|BIT(13))){r=LEO_SD_DATA;goto fail;}
 r=regwrite(c,CLEAR,0x07c007ffU);if(r)goto fail;
 r=regwrite(c,0x24,0xffffffffU);if(r)goto fail;
 r=regwrite(c,0x28,512U);if(r)goto fail;
 r=regwrite(c,0x2c,(512U<<4)|BIT(0)|BIT(1)|BIT(20));if(r)goto fail;
 r=command(c,17,arg,1,0,1,resp);if(r)goto stop;
 if(resp[0]&R1ERR){r=LEO_SD_PROTOCOL;goto stop;}
 begin=c->io.now_ns(c->io.cookie);
 for(;;) {
  status=rd(c,LEO_SDCC,STATUS);c->status=status;
  if(status&DATAERR){r=status&BIT(3)?LEO_SD_TIMEOUT:LEO_SD_DATA;break;}
  if(status&BIT(21)) {
   if(words==128U){r=LEO_SD_DATA;break;}
   word=rd(c,LEO_SDCC,0x80);for(i=0;i<4U;++i)bytes[words*4U+i]=(uint8_t)(word>>(8U*i));++words;
  }
  if((status&BIT(8))&&!(status&BIT(21))) {
   r=(words==128U&&rd(c,LEO_SDCC,0x30)==0U)?LEO_SD_OK:LEO_SD_SHORT;break;
  }
  if(expired(c,begin,2ULL*SECOND,++n)){r=LEO_SD_TIMEOUT;break;}
 }
stop:
 {int stopped=regwrite(c,0x2c,0);if(r==LEO_SD_OK)r=stopped;}
 if(r==LEO_SD_OK){
  if(sector==0U&&c->requested_clock_hz==144000U)c->slow_read0_ok=1;
  return r;
 }
fail:
 c->faulted=1;return r;
}
/* Write-programming readiness has a separate1s overall deadline (SD write
 * limits may extend to800ms). Existing read/start ready() stays unchanged.
 * CMD13 execution is bounded by that SAME deadline, including response polling. */
static int write_ready(struct leo_sd_card *c)
{
 uint64_t begin=c->io.now_ns(c->io.cookie);uint32_t n=0,status,resp[4];unsigned i;int r;
 for(;;){
  if(expired(c,begin,SECOND,n))return LEO_SD_TIMEOUT;
  c->command=13U;
  r=regwrite(c,0x0c,0);if(r)return r;
  r=regwrite(c,CLEAR,CMDERR|BIT(6)|BIT(7)|BIT(23));if(r)return r;
  if(expired(c,begin,SECOND,n))return LEO_SD_TIMEOUT;
  wr(c,LEO_SDCC,8,c->rca);wr(c,LEO_SDCC,0x0c,13U|BIT(6)|BIT(10));
  for(;;){
   status=rd(c,LEO_SDCC,STATUS);c->status=status;
   if(status&BIT(2))return LEO_SD_TIMEOUT;
   if(status&BIT(0))return LEO_SD_COMMAND;
   if(expired(c,begin,SECOND,++n))return LEO_SD_TIMEOUT;
   if(status&BIT(6))break;
  }
  for(i=0;i<4;++i)resp[i]=rd(c,LEO_SDCC,0x14U+4U*i);
  if(resp[0]&R1ERR)return LEO_SD_PROTOCOL;
  if((resp[0]&BIT(8))&&((resp[0]>>9)&15U)==4U)return LEO_SD_OK;
  r=delay(c,1000U);if(r)return r;
 }
}
int leo_sd_write_sector(struct leo_sd_card *c,uint64_t sector,const uint8_t bytes[512])
{
 uint32_t arg,resp[4],status,word,n=0,words=0;uint64_t begin;unsigned i;int r;
 if(!c||!bytes||sector>=c->sectors||!ios7lab_sd_command_address(sector,c->high_capacity,&arg))return LEO_SD_ARGUMENT;
 if(!c->initialized||c->faulted)return LEO_SD_FAULTED;
 if(c->csd_write_protected)return LEO_SD_WRITE_PROTECTED;
 ++c->write_attempts;c->write_words=0;c->stage=9;
 /* Own neither an inherited command nor a FIFO. Stop is not a FIFO flush. */
 r=regwrite(c,0x2c,0);if(r)goto fail;
 status=rd(c,LEO_SDCC,STATUS);c->status=status;
 if(status&(BIT(11)|BIT(12)|BIT(13)|BIT(20)|BIT(21))){r=LEO_SD_DATA;goto fail;}
 r=ready(c);if(r)goto fail;
 r=regwrite(c,CLEAR,0x07c007ffU);if(r)goto fail;
 r=regwrite(c,0x24,0xffffffffU);if(r)goto fail;
 r=regwrite(c,0x28,512U);if(r)goto fail;
 /* Qualcomm byte-count block field; TX direction, no DMA/READ_PENDING. */
 r=regwrite(c,0x2c,(512U<<4)|BIT(0));if(r)goto fail;
 r=command(c,24U,arg,1U,0,1,resp);if(r)goto stop;
 if(resp[0]&R1ERR){r=LEO_SD_PROTOCOL;goto stop;}
 begin=c->io.now_ns(c->io.cookie);
 for(;;) {
  status=rd(c,LEO_SDCC,STATUS);c->status=status;
  if(status&DATAERR){r=status&BIT(3)?LEO_SD_TIMEOUT:LEO_SD_DATA;break;}
  if(status&CMDERR){r=status&BIT(2)?LEO_SD_TIMEOUT:LEO_SD_COMMAND;break;}
  if(status&BIT(8)) {
   if(words!=128U||rd(c,LEO_SDCC,0x30U)!=0U){r=LEO_SD_SHORT;break;}
   if(!(status&BIT(20))&&(status&BIT(18))){r=LEO_SD_OK;break;}
  }else if((status&(BIT(14)|BIT(18)))&&!(status&BIT(16))&&words<128U) {
   word=0;for(i=0;i<4U;++i)word|=(uint32_t)bytes[words*4U+i]<<(8U*i);
   wr(c,LEO_SDCC,0x80U,word);c->write_words=++words;
  }
  if(expired(c,begin,2ULL*SECOND,++n)){r=LEO_SD_TIMEOUT;break;}
 }
stop:
 {int stopped=regwrite(c,0x2c,0);if(r==LEO_SD_OK)r=stopped;}
 if(r==LEO_SD_OK) {
  status=rd(c,LEO_SDCC,STATUS);c->status=status;
  if(status&(BIT(11)|BIT(12)|BIT(13)|BIT(20)|BIT(21)))r=LEO_SD_DATA;
  /* DATAEND is not card programming completion. Preserve real error/ready bits;
   * source-supported CMD13 must report ready-for-data and TRAN before success. */
  if(r==LEO_SD_OK)r=write_ready(c);
  if(r==LEO_SD_OK){++c->write_completed;c->write_result=LEO_SD_OK;return r;}
 }
fail:
 c->write_result=r;c->faulted=1;return r;
}
uint32_t leo_sd_csd_transfer_rate(const uint32_t words[4])
{
 static const uint32_t exponent[4]={10000U,100000U,1000000U,10000000U};
 static const uint8_t mantissa[16]={0,10,12,13,15,20,25,30,35,40,45,50,55,60,70,80};
 uint32_t raw,unit,multiplier;
 if(!words)return 0;
 raw=ios7lab_sd_csd_bits(words,103U,96U);unit=raw&7U;multiplier=(raw>>3)&15U;
 if((raw&0x80U)||unit>=4U||!multiplier)return 0;
 return exponent[unit]*mantissa[multiplier];
}
int leo_sd_transfer_clock(struct leo_sd_card *c,const uint8_t slow_reference[512])
{
 uint8_t verified[512];uint32_t status,global;unsigned i;int r;
 if(!c||!slow_reference)return LEO_SD_ARGUMENT;
 if(c->faulted||!c->initialized)return LEO_SD_FAULTED;
 if(c->rpc_uncertain)return LEO_SD_RPC_UNCERTAIN;
 if(!c->slow_read0_ok||c->requested_clock_hz!=144000U||c->transfer_clock_attempted)
  return LEO_SD_ARGUMENT;
 c->transfer_clock_attempted=1;
 c->csd_transfer_hz=leo_sd_csd_transfer_rate(c->csd);
 /* Only known default25MHz is requested, not arbitrary rounded/high-speed rates. */
 if(c->csd_transfer_hz<25000000U)return LEO_SD_OK;
 c->stage=7;
 status=rd(c,LEO_SDCC,STATUS);c->status=status;
 if(status&(BIT(11)|BIT(12)|BIT(13)|BIT(21))){r=LEO_SD_DATA;goto fail;}
 r=ready(c);if(r)goto fail;
 c->requested_clock_hz=25000000U;c->requested_selector=7U;
 r=rpc_values(c,50U,16U,0U);if(r)goto fail;
 r=rpc_values(c,58U,67U,7U);if(r)goto fail;
 r=rpc_values(c,50U,67U,0U);if(r)goto fail;
 global=rd(c,LEO_CLOCK,0);wr(c,LEO_CLOCK,0,global|BIT(8));
 if(!(rd(c,LEO_CLOCK,0)&BIT(8))){r=LEO_SD_CLOCK;goto fail;}
 /* Existing one-bit enable/flow/feedback and conservative50us delay preserved. */
 r=regwrite(c,4,BIT(8)|BIT(12)|BIT(15));if(r)goto fail;
 c->stage=8;r=ready(c);if(r)goto fail;
 r=leo_sd_read_sector(c,0U,verified);if(r)goto fail;
 for(i=0;i<512U;++i)if(verified[i]!=slow_reference[i]){r=LEO_SD_PROTOCOL;goto fail;}
 c->stage=8;c->fast_read0_verified=1;return LEO_SD_OK;
fail:
 /* Never clear uncertainty/faults, retry RPCs or quietly fall back after issue. */
 c->faulted=1;return r;
}
