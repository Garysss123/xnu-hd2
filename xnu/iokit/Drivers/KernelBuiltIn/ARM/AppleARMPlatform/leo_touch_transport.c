/* Owned Type2A transport. Hardware facts and ordering are crosschecked against
 * the Leo controller/register observations and HTC Linux/Cotulla board/I2C sources.
 * No foreign OS ABI, fixed virtual address, firmware, IRQ or button code.
 * Native execution is restricted to the caller's normal serialized workloop. */
#include "IOS7LeoTouchTransport.h"

#define I2C_WRITE 0x00U
#define I2C_CLOCK 0x04U
#define I2C_STATUS 0x08U
#define I2C_READ 0x0cU
#define I2C_SELECT 0x10U
#define WRITE_FULL 1U
#define READ_FULL 2U
#define ERROR_MASK 0xfcU
#define BUS_ACTIVE 0x100U
#define BUS_MASTER 0x200U
#define ADDRESS_BYTE 0x100U
#define LAST_BYTE 0x200U
#define SCL_HIGH 0x100U
#define RX_STATE (3U<<11)
#define TOUCH_ADDRESS 0x15U
#define TRANSFER_LIMIT_NS 20000000ULL
#define INIT_ATTEMPTS 17U

static uint32_t rd(LeoTouchState *s,uint32_t region,uint32_t off)
{return s->io.read(s->io.cookie,region,off);}
static void wr(LeoTouchState *s,uint32_t region,uint32_t off,uint32_t value)
{s->io.write(s->io.cookie,region,off,value);}
static int delay(LeoTouchState *s,uint32_t us)
{return s->io.delay_us(s->io.cookie,us)?LEO_TOUCH_OK:LEO_TOUCH_CLOCK_ERROR;}
static void diagnostic(LeoTouchState *s,uint32_t phase,uint32_t status)
{
 s->last_status=status;
 s->last_diagnostic=((phase&255U)<<24)|
  ((rd(s,LEO_TOUCH_I2C,I2C_SELECT)&0x300U)<<8)|(status&0xffffU);
}
static int deadline(LeoTouchState *s,uint32_t count)
{
 uint64_t now=s->io.now_ns(s->io.cookie);
 if(now<s->transfer_start_ns)return LEO_TOUCH_CLOCK_ERROR;
 if(count>=s->io.poll_bound || now-s->transfer_start_ns>=TRANSFER_LIMIT_NS)
  return LEO_TOUCH_TIMEOUT;
 return LEO_TOUCH_OK;
}
static int wait_status(LeoTouchState *s,uint32_t phase,uint32_t mask,
                       uint32_t want,int errors)
{
 uint32_t count=0,status;int r;
 for(;;){
  status=rd(s,LEO_TOUCH_I2C,I2C_STATUS);
  r=deadline(s,count++);
  if(r!=LEO_TOUCH_OK){diagnostic(s,phase,status);return r;}
  if(errors && (status&ERROR_MASK)){
   diagnostic(s,phase,status);return LEO_TOUCH_BUS_ERROR;
  }
  if((status&mask)==want)return LEO_TOUCH_OK;
 }
}
static int write_setup(LeoTouchState *s)
{
 if(!(rd(s,LEO_TOUCH_I2C,I2C_SELECT)&SCL_HIGH))return delay(s,6U);
 return LEO_TOUCH_OK;
}
static int address(LeoTouchState *s,int reading)
{
 int r=wait_status(s,2U,WRITE_FULL,0U,1);
 if(r!=LEO_TOUCH_OK)return r;
 r=write_setup(s);if(r!=LEO_TOUCH_OK)return r;
 wr(s,LEO_TOUCH_I2C,I2C_WRITE,ADDRESS_BYTE|(TOUCH_ADDRESS<<1)|(reading?1U:0U));
 return LEO_TOUCH_OK;
}
static int read_once(LeoTouchState *s,uint8_t *data,uint32_t bytes)
{
 uint32_t i;int r;
 if(!data || !bytes || bytes>9U)return LEO_TOUCH_ARGUMENT;
 s->transfer_start_ns=s->io.now_ns(s->io.cookie);
 r=wait_status(s,1U,BUS_ACTIVE,0U,0);
 if(r==LEO_TOUCH_OK)r=address(s,1);
 if(r!=LEO_TOUCH_OK)return r;
 if(bytes==1U){
  r=wait_status(s,3U,RX_STATE,RX_STATE,1);
  if(r!=LEO_TOUCH_OK)return r;
  wr(s,LEO_TOUCH_I2C,I2C_WRITE,LAST_BYTE);
 }
 for(i=0;i<bytes;++i){
  r=wait_status(s,4U,READ_FULL,READ_FULL,1);
  if(r!=LEO_TOUCH_OK)return r;
  /* QSD8x50 NACK/STOP must precede consuming the second-last byte. */
  if(bytes-i==2U)wr(s,LEO_TOUCH_I2C,I2C_WRITE,LAST_BYTE);
  data[i]=(uint8_t)rd(s,LEO_TOUCH_I2C,I2C_READ);
 }
 return wait_status(s,1U,BUS_ACTIVE,0U,0);
}
static int write_enable_once(LeoTouchState *s)
{
 static const uint8_t enable[3]={0xd0U,0U,1U};
 uint32_t i;int r;
 s->transfer_start_ns=s->io.now_ns(s->io.cookie);
 r=wait_status(s,1U,BUS_ACTIVE,0U,0);
 if(r==LEO_TOUCH_OK)r=address(s,0);
 for(i=0;r==LEO_TOUCH_OK && i<3U;++i){
  r=wait_status(s,2U,WRITE_FULL,0U,1);
  if(r!=LEO_TOUCH_OK)break;
  r=write_setup(s);if(r!=LEO_TOUCH_OK)break;
  wr(s,LEO_TOUCH_I2C,I2C_WRITE,enable[i]|(i==2U?LAST_BYTE:0U));
 }
 if(r==LEO_TOUCH_OK)r=wait_status(s,1U,BUS_ACTIVE,0U,0);
 return r;
}
static int recover(LeoTouchState *s)
{
 uint32_t status=rd(s,LEO_TOUCH_I2C,I2C_STATUS);int r;
 ++s->recoveries;
 s->transfer_start_ns=s->io.now_ns(s->io.cookie);
 if(status&READ_FULL){
  wr(s,LEO_TOUCH_I2C,I2C_WRITE,LAST_BYTE);
  (void)rd(s,LEO_TOUCH_I2C,I2C_READ);
 }else if(status&BUS_MASTER){
  wr(s,LEO_TOUCH_I2C,I2C_WRITE,LAST_BYTE|0xffU);
 }
 r=wait_status(s,5U,BUS_ACTIVE,0U,0);
 return r;
}
static int transfer(LeoTouchState *s,uint8_t *data,uint32_t bytes,int reading,
                    uint32_t attempts)
{
 uint32_t n,original_diag,original_status;int r,recovered;
 if(!attempts || attempts>INIT_ATTEMPTS)return LEO_TOUCH_ARGUMENT;
 for(n=0;n<attempts;++n){
  s->last_diagnostic=0;
  r=reading?read_once(s,data,bytes):write_enable_once(s);
  if(r==LEO_TOUCH_OK)return r;
  original_diag=s->last_diagnostic;original_status=s->last_status;
  recovered=recover(s);
  /* Recovery does not turn the failed transfer into a successful packet. */
  s->last_diagnostic=original_diag;s->last_status=original_status;
  if(recovered!=LEO_TOUCH_OK || n+1U==attempts)return r;
  recovered=delay(s,10U);if(recovered!=LEO_TOUCH_OK)return recovered;
 }
 return LEO_TOUCH_ARGUMENT;
}
static int gpio_bank(uint32_t pin,uint32_t *region,uint32_t *out,
                      uint32_t *oe,uint32_t *in,uint32_t *bit)
{
 if(pin==27U){*region=LEO_TOUCH_GPIO2;*out=0xc00U;*oe=0xc08U;*in=0xc20U;*bit=1U<<11;}
 else if(pin==82U || pin==92U){*region=LEO_TOUCH_GPIO1;*out=0x808U;*oe=0x828U;*in=0x858U;*bit=1U<<(pin-68U);}
 else if(pin==108U){*region=LEO_TOUCH_GPIO1;*out=0x810U;*oe=0x830U;*in=0x860U;*bit=1U<<4;}
 else if(pin==160U){*region=LEO_TOUCH_GPIO1;*out=0x818U;*oe=0x838U;*in=0x868U;*bit=1U<<7;}
 else return LEO_TOUCH_ARGUMENT;
 return LEO_TOUCH_OK;
}
static int gpio_write(LeoTouchState *s,uint32_t pin,int high,int direction)
{
 uint32_t region,out,oe,in,bit,value;int r=gpio_bank(pin,&region,&out,&oe,&in,&bit);
 if(r!=LEO_TOUCH_OK)return r;
 if(pin==92U){
  if(!direction)return LEO_TOUCH_ARGUMENT;
  wr(s,region,oe,rd(s,region,oe)&~bit);return LEO_TOUCH_OK;
 }
 value=rd(s,region,out);wr(s,region,out,high?(value|bit):(value&~bit));
 if(direction)wr(s,region,oe,rd(s,region,oe)|bit);
 return LEO_TOUCH_OK;
}
static int gpio_high(LeoTouchState *s,uint32_t pin)
{
 uint32_t region,out,oe,in,bit;
 if(gpio_bank(pin,&region,&out,&oe,&in,&bit)!=LEO_TOUCH_OK)return -1;
 return (rd(s,region,in)&bit)!=0;
}
static int ensure_gpio(LeoTouchState *s,uint32_t pin,int high)
{
 uint32_t n;int r;
 for(n=0;n<20U;++n){
  r=gpio_write(s,pin,high,0);if(r!=LEO_TOUCH_OK)return r;
  if(gpio_high(s,pin)==high)return LEO_TOUCH_OK;
  r=delay(s,1000U);if(r!=LEO_TOUCH_OK)return r;
 }
 s->last_diagnostic=pin;return LEO_TOUCH_GPIO_ERROR;
}
static int reset_panel(LeoTouchState *s)
{
 uint32_t n;int r;
 s->stage=LEO_TOUCH_STAGE_RESET;
 r=gpio_write(s,108U,1,0);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,160U,0,0);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,82U,0,0);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,27U,0,0);
 if(r==LEO_TOUCH_OK)r=delay(s,10000U);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,82U,1,0);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,27U,1,0);
 if(r==LEO_TOUCH_OK)r=ensure_gpio(s,160U,1);
 if(r==LEO_TOUCH_OK)r=delay(s,20000U);
 if(r!=LEO_TOUCH_OK)return r;
 /* Polling does not require a new GPIO IRQ. A high line after 500ms is only
  * an observation; the subsequent real ID/enable is the admission gate. */
 for(n=0;n<50U && gpio_high(s,92U)>0;++n){
  r=delay(s,10000U);if(r!=LEO_TOUCH_OK)return r;
 }
 r=ensure_gpio(s,108U,0);
 if(r==LEO_TOUCH_OK)r=delay(s,300000U);
 return r;
}
static int note_error(LeoTouchState *s,int r)
{
 s->last_result=r;++s->errors;if(r==LEO_TOUCH_TIMEOUT)++s->timeouts;
 return r;
}
int leo_touch_transport_init(LeoTouchState *s,const LeoTouchIO *io)
{
 uint32_t i,cycle,ns;uint8_t id[4];int r=LEO_TOUCH_ARGUMENT;
 if(!s || !io || !io->read || !io->write || !io->now_ns || !io->delay_us ||
    !io->poll_bound || io->poll_bound>262144U)return LEO_TOUCH_ARGUMENT;
 for(i=0;i<sizeof(*s);++i)((uint8_t *)s)[i]=0;
 s->io=*io;s->stage=LEO_TOUCH_STAGE_CONFIG;
 r=gpio_write(s,160U,0,1);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,108U,0,1);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,82U,0,1);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,27U,0,1);
 if(r==LEO_TOUCH_OK)r=gpio_write(s,92U,0,1);
 if(r!=LEO_TOUCH_OK)return note_error(s,r);
 for(cycle=0;cycle<3U;++cycle){
  s->init_cycles=cycle+1U;
  r=reset_panel(s);if(r!=LEO_TOUCH_OK)return note_error(s,r);
  s->stage=LEO_TOUCH_STAGE_CLOCK;
  ns=rd(s,LEO_TOUCH_CLOCK,0x64U);
  wr(s,LEO_TOUCH_CLOCK,0x64U,(ns&0xfffff000U)|0x0a00U);
  wr(s,LEO_TOUCH_I2C,I2C_SELECT,0U);
  wr(s,LEO_TOUCH_I2C,I2C_CLOCK,0x35dU);
  s->stage=LEO_TOUCH_STAGE_ID;
  for(i=0;i<4U;++i)id[i]=0;
  r=transfer(s,id,4U,1,INIT_ATTEMPTS);
  if(r!=LEO_TOUCH_OK)continue;
  s->id_word=((uint32_t)id[0]<<24)|((uint32_t)id[1]<<16)|((uint32_t)id[2]<<8)|id[3];
  if(id[0]!=0x55U){r=LEO_TOUCH_PROTOCOL;s->last_diagnostic=s->id_word;continue;}
  s->stage=LEO_TOUCH_STAGE_ENABLE;
  r=transfer(s,0,0,0,INIT_ATTEMPTS);
  if(r!=LEO_TOUCH_OK)return note_error(s,r);
  s->ready=1;s->stage=LEO_TOUCH_STAGE_READY;s->last_result=LEO_TOUCH_OK;
  return LEO_TOUCH_OK;
 }
 return note_error(s,r);
}
static uint16_t scale(uint32_t value,uint32_t lower,uint32_t upper,uint32_t pixels)
{
 if(value<lower)value=lower;
 if(value>upper)value=upper;
 return (uint16_t)(((value-lower)*(pixels-1U))/(upper-lower));
}
int leo_touch_transport_poll(LeoTouchState *s,LeoTouchReport *report)
{
 LeoTouchReport next;uint8_t packet[9];uint32_t i,contacts,x,y;int r;
 if(!s || !report)return LEO_TOUCH_ARGUMENT;
 if(!s->ready)return LEO_TOUCH_NOT_READY;
 ++s->polls;
 if(gpio_high(s,92U)>0){
  ++s->not_ready_skips;
  if(++s->high_polls<10U)return 0;
 }else s->high_polls=0;
 s->high_polls=0;s->stage=LEO_TOUCH_STAGE_READ;++s->reads;
 r=transfer(s,packet,9U,1,1U);
 if(r!=LEO_TOUCH_OK)return note_error(s,r);
 contacts=(packet[8]>>1)&3U;
 if(packet[0]!=0x5aU || contacts>2U){
  s->last_diagnostic=((uint32_t)packet[0]<<24)|packet[8];
  return note_error(s,LEO_TOUCH_PROTOCOL);
 }
 next.contacts=(uint8_t)contacts;next.down=contacts!=0U;
 x=packet[2]|((uint32_t)(packet[1]&0xf0U)<<4);
 y=packet[3]|((uint32_t)(packet[1]&0x0fU)<<8);
 next.x=contacts?scale(x,6U,576U,480U):s->last_x;
 next.y=contacts?scale(y,6U,952U,800U):s->last_y;
 next.sequence=++s->sequence;
 for(i=0;i<9U;++i){next.raw[i]=packet[i];s->last_raw[i]=packet[i];}
 if(contacts && !s->last_contacts)++s->presses;
 if(!contacts && s->last_contacts)++s->releases;
 s->last_contacts=contacts;s->last_x=next.x;s->last_y=next.y;
 ++s->packets;s->stage=LEO_TOUCH_STAGE_REPORT;s->last_result=LEO_TOUCH_OK;
 *report=next;
 return 1;
}
