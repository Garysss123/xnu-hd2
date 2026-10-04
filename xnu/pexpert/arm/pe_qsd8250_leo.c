/* iOS-lab owned first-entry-only HTC Leo PE.
 * Genuine XNU early control flow; no UART, interrupt, timer, SD or UI success
 * is fabricated. Loader state is revalidated before native RGB565 writes. */
#if defined(BOARD_CONFIG_QSD8250_LEO)
#include <mach/mach_types.h>
#include <mach/host_info.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm/boot.h>
#include <pexpert/arm/protos.h>
#include <pexpert/arm/leo_handoff.h>
#include <pexpert/arm/leo_entry.h>
#include "leo_qsd8250_timer.h"
#include <machine/machine_routines.h>
#include <kern/debug.h>
#include <kern/clock.h>
#include <mach/vm_param.h>
#include "IOS7LeoDisplayBackend.h"
#include "leo_scanout.h"
#include "IOS7LeoDisplayObservation.h"
#include "IOS7LeoMDPFault.h"
#include "IOS7LeoMemoryObservation.h"
#include "IOS7LeoMemoryPolicy.h"
#include "IOS7LeoFunctionalContext.h"
/* Exact console/serial_protos.h ABI; not exported in the PE include set. */
extern int switch_to_serial_console(void);
/* Existing osfmk host.c accessor; typed public VM statistics, no private
 * processor_data layout or guessed offset is consumed by this PE TU. */
extern host_t host_self(void);
extern kern_return_t host_statistics64(host_t host, host_flavor_t flavor,
                                       host_info64_t info,
                                       mach_msg_type_number_t *count);

typedef char leo_handoff_size[(sizeof(ios7lab_leo_handoff) == 128) ? 1 : -1];
extern uint64_t sane_size;
extern unsigned int vm_page_free_count,vm_page_free_reserved,vm_page_free_target;
static volatile LeoMemoryObservation leo_memory_state;
static volatile unsigned int leo_memory_sequence;
static volatile unsigned short *leo_pixels;
static vm_offset_t leo_mdp;
static unsigned int leo_fb_physical;
static volatile unsigned int leo_graphics_owner,leo_graphics_faulted,leo_refresh_pending;
static uint64_t leo_graphics_sequence;
/* A timeout owns one already-kicked DMA until real DONE is acknowledged.
 * It never owns a successful caller fence or authorizes another kick. */
static volatile unsigned int leo_graphics_late_pending;
static volatile LeoMDPFaultObservation leo_mdp_fault_state;
/* Observer counters only; no presentation or refresh control is read from them. */
static volatile uint32_t leo_obs_attempts,leo_obs_rows,leo_obs_last_rows,leo_obs_completed;
static volatile uint32_t leo_obs_failures,leo_obs_failure_phase,leo_obs_refresh_requests;
static volatile uint32_t leo_obs_owner_blocks,leo_obs_pending_skips,leo_obs_normal_kicks,leo_obs_graphics_kicks;
static volatile uint32_t leo_obs_normal_at_claim,leo_obs_normal_after_owner;
static int leo_obs_failed(uint32_t phase)
{ ++leo_obs_failures;leo_obs_failure_phase=phase;return 0; }
static unsigned int leo_stage;
static volatile unsigned int leo_final_reason;
static volatile unsigned int leo_console_count;
static volatile char leo_console[1024];
static volatile unsigned int leo_fatal_active;
static unsigned int leo_fatal_category;
/* Exact captured fields only. Mask1=PC,2=LR,4=FAR,8=FSR; unknown is FFFFFFFF. */
static volatile unsigned int leo_fatal_state[8];

static unsigned int leo_read(unsigned int offset)
{ return *(volatile unsigned int *)(leo_mdp + offset); }
static void leo_write(unsigned int offset, unsigned int value)
{ *(volatile unsigned int *)(leo_mdp + offset) = value; }
static void leo_sync(void)
{ __asm__ volatile("dsb sy" ::: "memory"); }

/* Fixed four-bit hex glyphs, seven rows; one80x40 channel at(8,8). */
static const unsigned char leo_hex[16][7] = {
 {6,9,9,9,9,9,6},{2,6,2,2,2,2,7},{6,9,1,2,4,8,15},{14,1,1,6,1,1,14},
 {2,6,10,10,15,2,2},{15,8,8,14,1,1,14},{6,8,8,14,9,9,6},{15,1,2,2,4,4,4},
 {6,9,9,6,9,9,6},{6,9,9,7,1,1,6},{6,9,9,15,9,9,9},{14,9,9,14,9,9,14},
 {7,8,8,8,8,8,7},{14,9,9,9,9,9,14},{15,8,8,14,8,8,15},{15,8,8,14,8,8,8}
};
static void leo_digit(unsigned int digit, unsigned int x, unsigned int y,
                      unsigned short color)
{
 unsigned int row, bit, a, b;
 for (row=0;row<7;row++) for (bit=0;bit<4;bit++)
  if (leo_hex[digit & 15][row] & (8U >> bit))
   for (a=0;a<2;a++) for (b=0;b<2;b++)
    leo_pixels[(y+row*2+a)*480+x+bit*2+b]=color;
}
static void leo_number(unsigned int value, unsigned int digits, unsigned int y,
                       unsigned short color)
{
 unsigned int i;
 for(i=0;i<digits;i++)
  leo_digit((value >> ((digits-i-1)*4)) & 15,8+i*10,y,color);
}
static const unsigned char leo_alpha[26][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {15,16,16,19,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {1,1,1,1,17,17,14},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};
static void leo_text(const char *s, unsigned int limit, unsigned int x,
                     unsigned int y)
{
 unsigned int n,row,bit,a,b;unsigned char c;
 const unsigned char *punct;
 static const unsigned char dot[7]={0,0,0,0,0,12,12};
 static const unsigned char colon[7]={0,12,12,0,12,12,0};
 static const unsigned char dash[7]={0,0,0,31,0,0,0};
 static const unsigned char slash[7]={1,2,2,4,8,8,16};
 static const unsigned char under[7]={0,0,0,0,0,0,31};
 static const unsigned char left[7]={2,4,8,8,8,4,2};
 static const unsigned char right[7]={8,4,2,2,2,4,8};
 static const unsigned char question[7]={14,17,1,2,4,0,4};
 for(n=0;n<limit;n++) {
  c=(unsigned char)s[n];if(!c)break;
  if(c>='a' && c<='z') c=(unsigned char)(c-'a'+'A');
  if(c>='A' && c<='Z') {
   for(row=0;row<7;row++)for(bit=0;bit<5;bit++)
    if(leo_alpha[c-'A'][row]&(16U>>bit))
     for(a=0;a<2;a++)for(b=0;b<2;b++)
      leo_pixels[(y+row*2+a)*480+x+n*12+bit*2+b]=0xffff;
  } else if(c>='0' && c<='9') leo_digit(c-'0',x+n*12,y,0xffff);
  else if(c!=' ') {
   punct= c=='.'?dot:c==':'?colon:c=='-'?dash:c=='/'?slash:
          c=='_'?under:c=='('?left:c==')'?right:question;
   for(row=0;row<7;row++)for(bit=0;bit<5;bit++)
    if(punct[row]&(16U>>bit))for(a=0;a<2;a++)for(b=0;b<2;b++)
     leo_pixels[(y+row*2+a)*480+x+n*12+bit*2+b]=0xffff;
  }
 }
}
static int leo_ack_refresh(void)
{
 /* A timed-out graphics request is retired only by the serialized graphics
  * worker, after configuration validation and verified W1C. */
 if(leo_graphics_late_pending)return 0;
 if(!leo_refresh_pending)return 1;
 if(leo_fatal_active)return 0;
 leo_sync();unsigned int done=leo_read(0x24);leo_sync();
 if(!(done&(1U<<14)))return 0;
 leo_write(0x28,1U<<14);leo_sync();leo_refresh_pending=0;return 1;
}
static int leo_drain_refresh(void)
{
 /* Normal graphics worker only, after actualclock initialized. Nevercalled
  * by IRQ/earlyconsole refresh; those callers perform oneack/skip only. */
 uint64_t begin=mach_absolute_time(),interval=0;
 (nanoseconds_to_absolutetime)(100000000ULL,&interval);
 for(unsigned int polls=0;polls<1000000U;polls++){
  if(leo_fatal_active)return 0;
  if(leo_ack_refresh())return 1;
  if(mach_absolute_time()-begin>=interval)break;
 }
 return 0;
}

static void leo_refresh(void)
{
 if(!leo_mdp)return;
 if(leo_fatal_active){leo_sync();leo_write(0x44,0U);leo_sync();return;}
 ++leo_obs_refresh_requests;
 /* At most one tracked normal DMA request; no queue of unrelated completion
  * sources. Fatal bypasses this tracking and remains the primary owner. */
 if(!leo_fatal_active && !leo_ack_refresh()){++leo_obs_pending_skips;return;}
 leo_sync();leo_write(0x28,1U<<14);leo_sync();
 if(!leo_fatal_active && (leo_read(0x24)&(1U<<14)))return;
 leo_write(0x44,0U);leo_sync();
 ++leo_obs_normal_kicks;
 if(leo_graphics_owner)++leo_obs_normal_after_owner;
 if(!leo_fatal_active)leo_refresh_pending=1;
}

static void leo_render(unsigned int address, unsigned int stage)
{
 unsigned int y,x;
 if (!leo_pixels || !leo_mdp) return;
 for(y=8;y<48;y++) for(x=8;x<88;x++) leo_pixels[y*480+x]=0;
 leo_number(address,8,8,0x07ff);
 leo_number(stage,4,28,0xffff);
 leo_refresh();
}
static unsigned int leo_callsite(void *returned)
{ return (((unsigned int)returned) & ~1U) - 4U; }

/* One final physical/fatal channel. Reason0=entry-only boundary, not OS boot.
 * No screen write is attempted if the tuple/mapping was never validated. */
int ios7leo_fatal_report(unsigned int category, unsigned int callsite,
 unsigned int pc,unsigned int lr,unsigned int far,unsigned int fsr,
 unsigned int valid_mask,const char *message)
{
 unsigned int x,y,n,hash=2166136261U;const char *labels[4]={"PC","LR","FAR","FSR"};
 unsigned int values[4]={pc,lr,far,fsr};
 if(leo_fatal_active)return 0;
 leo_fatal_active=1;
 /* Once the actual RAM sink exists, normal console output must not overwrite
  * the first red report. Original panic formatter/control flow is retained. */
 if(gPESocDispatch.uart_putc==ios7leo_debug_putc)
  (void)switch_to_serial_console();
 leo_fatal_category=category; /* latch first error; normal checkpoints cannot overwrite */
 leo_fatal_state[0]=category;leo_fatal_state[1]=leo_stage;
 leo_fatal_state[2]=valid_mask&15U;leo_fatal_state[3]=pc;
 leo_fatal_state[4]=lr;leo_fatal_state[5]=far;leo_fatal_state[6]=fsr;
 if(!leo_pixels || !leo_mdp)return 1;
 /* Bounded local RGB565/MMIO only: no locks/heap/FS/context probing or wait. */
 for(y=0;y<800;y++)for(x=0;x<480;x++)leo_pixels[y*480+x]=0xf800;
 leo_render(callsite,category);
 leo_text("RETURN",6,120,8); /* return address is not a captured faultPC */
 leo_text("XNU FATAL",9,8,60);
 leo_number(leo_stage,8,90,0xffff);
 /* Number columns are x8 by common channel; labels go to x120 to avoid overlap. */
 leo_text("STAGE",5,120,90);
 leo_number(valid_mask&15U,8,120,0xffff);leo_text("VALID MASK",10,120,120);
 for(n=0;n<4;n++) {
  leo_number((valid_mask&(1U<<n))?values[n]:0xffffffffU,8,150+n*30,0xffff);
  leo_text(labels[n],3,120,150+n*30);
 }
 leo_refresh(); /* Primary actual category/fields first, before message reads. */
 if(message) {
  for(n=0;n<64 && message[n];n++) hash=(hash^(unsigned char)message[n])*16777619U;
  leo_text("FORMAT",6,120,290);
  leo_number(hash,8,290,0xffff);leo_fatal_state[7]=hash;
  leo_text(message,38,8,320); /* bounded raw original format; not formatted args */
 }
 leo_refresh();
 /* Return primary-owner token; original fatal flow is never swallowed. */
 return 1;
}
static volatile unsigned int leo_panic_text_mode,leo_panic_text_len;
static volatile unsigned int leo_panic_text_truncated;
static char leo_panic_text[384];
static unsigned int leo_panic_reason,leo_panic_reason_known;
/* First formatter owns capture; nested panic uses original sink directly. */
int ios7leo_panic_text_begin(unsigned int reason,unsigned int reason_known,unsigned int primary)
{
 if(!primary || !leo_fatal_active || leo_panic_text_mode ||
    (leo_fatal_category!=0xf101U && leo_fatal_category!=0xf103U))return 0;
 leo_panic_text_mode=1;leo_panic_reason=reason;
 leo_panic_reason_known=reason_known;return 1;
}
void ios7leo_panic_text_putc(char c)
{
 extern void ios7leo_log_putc(char);
 if(leo_panic_text_mode!=1)return;
 ios7leo_log_putc(c); /* nonblocking enqueue only; no fatal SD flush */
 if(leo_panic_text_len<sizeof(leo_panic_text))
  leo_panic_text[leo_panic_text_len++]=c;
 else leo_panic_text_truncated=1;
}
void ios7leo_panic_text_end(void)
{
 unsigned int x,y,line,n,at=0;char part[39];
 if(leo_panic_text_mode!=1)return;
 leo_panic_text_mode=2;
 if(!leo_pixels || !leo_mdp)return;
 for(y=280;y<620;y++)for(x=0;x<480;x++)leo_pixels[y*480+x]=0xf800;
 leo_text("FORMATTED PANIC",15,8,280);
 leo_number(leo_panic_reason_known?leo_panic_reason:0xffffffffU,8,310,0xffff);
 leo_text("CONTEXT REASON",14,120,310);
 for(line=0;line<12 && at<leo_panic_text_len;line++) {
  for(n=0;n<38 && at<leo_panic_text_len && leo_panic_text[at]!='\n';at++) {
   char c=leo_panic_text[at];if(c!='\r')part[n++]=c;
  }
  part[n]=0;leo_text(part,n,8,340+line*20);
  if(at<leo_panic_text_len && leo_panic_text[at]=='\n')at++;
 }
 if(at<leo_panic_text_len || leo_panic_text_truncated)
  leo_text("TEXT TRUNCATED",14,8,600);
 leo_refresh();
 /* Original formatter/sink/Debugger and fatal outcome are not replaced. */
}

static void leo_normal_frame(unsigned int callsite, const char *label,
                             unsigned int label_bytes)
{
 /* Ordinary output belongs to the real full-height generic console. */
 (void)callsite;(void)label;(void)label_bytes;
 if(leo_pixels && leo_mdp && !leo_fatal_active && !leo_graphics_owner)leo_refresh();
}

static unsigned int leo_continuation_active,leo_timer_observed,leo_last_tick;
void ios7leo_kernel_continuing(void)
{
 leo_normal_frame(leo_callsite(__builtin_return_address(0)),
                  "KERNEL CONTINUING",17);
 if(leo_pixels && leo_mdp && !leo_fatal_active)leo_continuation_active=1;
}

void ios7leo_timer_observe(unsigned int ticks,unsigned int irq7_count)
{
 (void)irq7_count;
 if(!leo_continuation_active || leo_fatal_active || leo_graphics_owner || !leo_pixels || !leo_mdp){
  if(leo_graphics_owner)++leo_obs_owner_blocks;
  return;
 }
 /* Keep actual32768Hz wrap-safe refresh, without an independent pixel writer.
  * GPT/VIC dispatch and IRQ counting remain in their original real paths. */
 if(leo_timer_observed && (unsigned int)(ticks-leo_last_tick)<32768U)return;
 leo_last_tick=ticks;leo_timer_observed=1;
 leo_refresh();
}

void __attribute__((noreturn)) ios7leo_final_boundary(unsigned int reason)
{
 leo_final_reason=reason;
 if(reason) ios7leo_fatal_report(0xf000U|(reason&0xfffU),
    leo_callsite(__builtin_return_address(0)),0,0,0,0,0,"ENTRY GUARD");
 else leo_normal_frame(leo_callsite(__builtin_return_address(0)),
                       "ENTRY CHECKPOINT",16);
 __asm__ volatile("cpsid if" ::: "memory");
 for (;;) __asm__ volatile("dsb sy\n\twfi" ::: "memory");
}

/* The installed 004 path is the functional ARM-B prefix.  Its validated
 * context lives at runtime_base+0xc000 and contains the parsed bank list and
 * final plan.  Read only that exact retained context through ml_io_map; never
 * dereference the physical pointer or search arbitrary RAM. */
static int leo_memory_map_copy(uint32_t physical,uint32_t bytes,void *dst)
{
 uint32_t base,end,map_bytes,offset;
 vm_offset_t mapped;
 volatile const uint8_t *src;
 uint8_t *out=(uint8_t *)dst;
 unsigned int i;
 if(!bytes || physical>0xffffffffU-bytes)return 0;
 base=physical&~4095U;offset=physical-base;
 if(offset>0xffffffffU-bytes || offset+bytes>0xffffffffU-4095U)return 0;
 if(base>0xffffffffU-(offset+bytes+4095U))return 0;
 end=(base+offset+bytes+4095U)&~4095U;
 if(end<base)return 0;
 map_bytes=end-base;
 if(!map_bytes || map_bytes>8192U)return 0;
 mapped=ml_io_map(base,map_bytes);
 if(!mapped)return 0;
 src=(volatile const uint8_t *)(mapped+offset);
 for(i=0;i<bytes;i++)out[i]=src[i];
 /* ARM machine_routines exposes ml_io_map but no matching unmap API.  The
  * two bounded prefix mappings are retained; no later physical range is read. */
 return 1;
}
static int leo_memory_range_end(uint32_t base,uint32_t bytes,uint32_t *end)
{
 if(!bytes || base>0xffffffffU-bytes)return 0;
 *end=base+bytes;return 1;
}
static int leo_memory_range_contains(uint32_t base,uint32_t bytes,
                                     uint32_t inner,uint32_t inner_bytes)
{
 uint32_t end,inner_end;
 return leo_memory_range_end(base,bytes,&end) &&
        leo_memory_range_end(inner,inner_bytes,&inner_end) &&
        inner>=base && inner_end<=end;
}
static int leo_memory_ranges_overlap(uint32_t a,uint32_t ab,uint32_t b,uint32_t bb)
{
 uint32_t ae,be;
 if(!leo_memory_range_end(a,ab,&ae) || !leo_memory_range_end(b,bb,&be))return 1;
 return a<be && b<ae;
}
static void leo_memory_state_clear(uint32_t status)
{
 volatile uint8_t *p=(volatile uint8_t *)&leo_memory_state;
 unsigned int i;
 for(i=0;i<sizeof(leo_memory_state);i++)p[i]=0;
 leo_memory_state.version=IOS7_LEO_MEMORY_OBS_VERSION;
 leo_memory_state.status=status;
}
static int leo_memory_context_header_ok(const ios7lab_leo_handoff *h,
                                        uint32_t *total_bytes)
{
 uint32_t end;
 if(!h || h->magic!=IOS7LAB_LEO_MAGIC || h->version!=IOS7LAB_LEO_VERSION ||
    h->bytes!=IOS7LAB_LEO_HANDOFF_BYTES || h->flags!=IOS7LAB_LEO_FLAGS ||
    h->machine!=IOS7LAB_LEO_MACHINE ||
    (h->entry_sctlr&IOS7LAB_LEO_SCTLR_REJECT) ||
    (h->entry_cpsr&0x23fU)!=0x13U)return 0;
 if(h->loader_end<=h->loader_start ||
    h->loader_end-h->loader_start!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    !leo_memory_range_end(h->loader_start,h->loader_end-h->loader_start,&end))return 0;
 if(h->loader_start!=h->initrd_start || h->loader_end>h->initrd_end)return 0;
 if(h->loader_start>0xffffffffU-IOS7_LEO_FUNCTIONAL_CONTEXT_OFFSET-
                         IOS7_LEO_FUNCTIONAL_CONTEXT_BYTES)return 0;
 if(!leo_memory_range_end(h->initrd_start,h->initrd_end-h->initrd_start,&end) ||
    h->initrd_start==0 || h->initrd_end<=h->initrd_start)return 0;
 if(h->entry_atags<IOS7_LEO_FUNCTIONAL_ATAG_LOW ||
    h->entry_atags>=IOS7_LEO_FUNCTIONAL_ATAG_HIGH || (h->entry_atags&3U) ||
    h->atags_end<=h->entry_atags ||
    h->atags_end>IOS7_LEO_FUNCTIONAL_ATAG_HIGH ||
    h->atags_end-h->entry_atags>IOS7_LEO_FUNCTIONAL_ATAG_LIMIT)return 0;
 if(!leo_memory_range_end(h->memory_base,h->memory_bytes,&end) ||
    (h->memory_base&0xfffffU) || (h->memory_bytes&0xfffffU) ||
    h->memory_bytes>0x40000000U)return 0;
 *total_bytes=h->initrd_end-h->initrd_start;
 if(!*total_bytes || *total_bytes>0x01000000U)return 0;
 return 1;
}
static int leo_memory_capture_functional(const ios7lab_leo_handoff *h)
{
 uint32_t total,context_pa,context_end,header[8],i,j,raw_plan=0,profile=0;
 LeoFunctionalContext c;
 leo_memory_state_clear(LEO_MEMORY_STATUS_UNKNOWN);
 if(!leo_memory_context_header_ok(h,&total)){
  leo_memory_state.status=LEO_MEMORY_STATUS_BAD_HANDOFF;return 0;
 }
 context_pa=h->loader_start+IOS7_LEO_FUNCTIONAL_CONTEXT_OFFSET;
 context_end=context_pa+IOS7_LEO_FUNCTIONAL_CONTEXT_BYTES;
 if(context_end>h->loader_end ||
    !leo_memory_map_copy(h->loader_start,32U,header) ||
    !leo_memory_map_copy(context_pa,sizeof(c),&c)){
  leo_memory_state.status=LEO_MEMORY_STATUS_MAP_FAILED;return 0;
 }
 leo_memory_state.status=LEO_MEMORY_STATUS_PARSE_FAILED;
 if(header[1]!=IOS7_LEO_FUNCTIONAL_MAGIC || header[2]!=IOS7_LEO_FUNCTIONAL_VERSION ||
    header[3]!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    header[4]<IOS7_LEO_FUNCTIONAL_PREFIX_BYTES || header[4]>total ||
    total-header[4]>=4096U || header[5]!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    header[6]>total-IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    header[4]!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES+header[6] ||
    c.r1!=IOS7_LEO_FUNCTIONAL_MACHINE || c.r2!=h->entry_atags ||
    c.runtime_base!=h->loader_start || c.total_bytes!=total ||
    c.prefix_bytes!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    c.container_offset!=IOS7_LEO_FUNCTIONAL_PREFIX_BYTES ||
    c.container_bytes!=header[6] ||
    c.phase!=5U || c.error!=0U ||
    c.tags.atags_base!=h->entry_atags ||
    c.tags.atags_bytes!=h->atags_end-h->entry_atags ||
    c.tags.initrd.base!=h->initrd_start ||
    c.tags.initrd.bytes!=total)return 0;
 if(c.tags.banks==0U || c.tags.banks>IOS7_LEO_FUNCTIONAL_MAX_BANKS ||
    c.plan.phys!=h->memory_base || c.plan.mem!=h->memory_bytes ||
    c.plan.top!=h->top_of_kernel_data ||
    c.plan.kernel_bytes!=h->kernel_extent ||
    !leo_memory_range_end(c.plan.phys,c.plan.mem,&context_end) ||
    c.plan.reserved_end>context_end)return 0;
 for(i=0;i<c.tags.banks;i++){
  uint32_t bank_end;
  if(!leo_memory_range_end(c.tags.bank[i].base,c.tags.bank[i].bytes,&bank_end) ||
     bank_end<=c.tags.bank[i].base)return 0;
  for(j=0;j<i;j++)if(leo_memory_ranges_overlap(c.tags.bank[i].base,
                                                 c.tags.bank[i].bytes,
                                                 c.tags.bank[j].base,
                                                 c.tags.bank[j].bytes))return 0;
 }
 for(i=0;i<c.tags.banks;i++)if(leo_memory_range_contains(c.tags.bank[i].base,
                                      c.tags.bank[i].bytes,h->entry_atags,
                                      h->atags_end-h->entry_atags))break;
 if(i==c.tags.banks)return 0;
 for(i=0;i<c.tags.banks;i++)if(leo_memory_range_contains(c.tags.bank[i].base,
                                      c.tags.bank[i].bytes,h->loader_start,
                                      h->loader_end-h->loader_start))break;
 if(i==c.tags.banks)return 0;
 for(i=0;i<c.tags.banks;i++)if(leo_memory_range_contains(c.tags.bank[i].base,
                                      c.tags.bank[i].bytes,h->initrd_start,total))break;
 if(i==c.tags.banks)return 0;
 for(i=0;i<c.tags.banks;i++)if(leo_memory_range_contains(c.tags.bank[i].base,
                                      c.tags.bank[i].bytes,c.plan.phys,c.plan.mem)){
  raw_plan=1;break;
 }
 if(!raw_plan){
  IOS7LeoMemoryPolicyRange initrd={c.tags.initrd.base,c.tags.initrd.bytes};
  IOS7LeoMemoryPolicyRange atags={c.tags.atags_base,c.tags.atags_bytes};
  IOS7LeoMemoryPolicyRange loader={h->loader_start,h->loader_end-h->loader_start};
  IOS7LeoMemoryPolicyRange plan={c.plan.phys,c.plan.mem};
  profile=ios7leo_memory_policy_expected_plan(c.tags.banks,
             c.tags.bank[0].base,c.tags.bank[0].bytes,
             initrd,atags,loader,plan);
  if(profile!=IOS7_LEO_MEMORY_PROFILE_LEGACY64)return 0;
 }
 leo_memory_state.atag_base=h->entry_atags;
 leo_memory_state.atag_end=h->atags_end;
 leo_memory_state.atag_bytes=h->atags_end-h->entry_atags;
 leo_memory_state.bank_count=c.tags.banks;
 for(i=0;i<c.tags.banks;i++){
  leo_memory_state.banks[i].base=c.tags.bank[i].base;
  leo_memory_state.banks[i].bytes=c.tags.bank[i].bytes;
 }
 leo_memory_state.selected_base=c.plan.phys;
 leo_memory_state.selected_bytes=c.plan.mem;
 leo_memory_state.known=LEO_MEMORY_KNOWN_ATAG |
   (profile?LEO_MEMORY_KNOWN_BOARD_PROFILE:0U);
 leo_memory_state.status=LEO_MEMORY_STATUS_OK;
 leo_memory_state.sequence=++leo_memory_sequence;
 return 1;
}
static void leo_memory_update_counters(LeoMemoryObservation *out)
{
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 out->managed_bytes=sane_size;
 out->free_pages=vm_page_free_count;
 out->free_reserved=vm_page_free_reserved;
 out->free_target=vm_page_free_target;
  vm_statistics64_data_t stat;mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
  if(host_statistics64(host_self(),HOST_VM_INFO64,(host_info64_t)&stat,&count)==KERN_SUCCESS &&
     count>=HOST_VM_INFO64_COUNT){
   out->pageins=stat.pageins;out->pageouts=stat.pageouts;out->faults=stat.faults;
   out->known|=LEO_MEMORY_KNOWN_VM_COUNTERS;
  }else if(out->status==LEO_MEMORY_STATUS_OK){
  out->status=LEO_MEMORY_STATUS_COUNTERS_PARTIAL;
 }
 ml_set_interrupts_enabled(enabled);
}
void ios7leo_memory_observe(LeoMemoryObservation *out)
{
 if(!out || out->version!=IOS7_LEO_MEMORY_OBS_VERSION)return;
 *out=*(const LeoMemoryObservation *)&leo_memory_state;
 leo_memory_update_counters(out);
}

static int leo_video_ok(const boot_args *args)
{
 unsigned long long end=(unsigned long long)args->Video.v_baseAddr+768000U;
 unsigned long long ramend=(unsigned long long)args->physBase+args->memSize;
 unsigned long long vaend=(unsigned long long)args->virtBase+args->memSize;
 return args->Revision==1 && args->Version==2 && args->virtBase==0x80000000U &&
  args->memSize && !(args->physBase & 0xfffffU) && !(args->memSize & 0xfffffU) &&
  args->memSize<=0x40000000U && ramend<=0x100000000ULL &&
  vaend<=IOS7LAB_LEO_MDP_PHYSICAL &&
  args->topOfKernelData>=args->physBase && !(args->topOfKernelData & 0x3fffU) &&
  (unsigned long long)args->topOfKernelData+IOS7LAB_LEO_EARLY_VM_RESERVE<=ramend &&
  args->machineType==IOS7LAB_LEO_MACHINE &&
  args->Video.v_width==480 && args->Video.v_height==800 &&
  args->Video.v_rowBytes==960 && args->Video.v_depth==16 &&
  !(args->Video.v_baseAddr & 3U) &&
  args->Video.v_baseAddr>=IOS7LAB_LEO_EARLY_FB_LOW &&
  end<=IOS7LAB_LEO_EARLY_FB_HIGH &&
  (ramend<=IOS7LAB_LEO_EARLY_FB_LOW || args->physBase>=IOS7LAB_LEO_EARLY_FB_HIGH) &&
  (ramend<=IOS7LAB_LEO_MDP_PHYSICAL ||
   args->physBase>=IOS7LAB_LEO_MDP_PHYSICAL+IOS7LAB_LEO_MDP_MAP_BYTES);
}
static int leo_dma_ok(void)
{
 return (leo_read(0x90000)&0x06003f80U)==0x02002100U &&
  leo_read(0x90004)==((800U<<16)|480U) &&
  leo_read(0x90008)==leo_fb_physical && leo_read(0x9000c)==960 &&
  leo_read(0x90010)==0;
}
int ios7leo_display_ready(void)
{
 return leo_pixels && leo_mdp && !leo_fatal_active && !leo_graphics_faulted &&
  PE_state.video.v_width==480 && PE_state.video.v_height==800 &&
  PE_state.video.v_rowBytes==960 && PE_state.video.v_depth==16 && leo_dma_ok();
}
int ios7leo_display_completed(uint64_t sequence)
{
 return sequence && sequence<=leo_graphics_sequence && !leo_fatal_active && !leo_graphics_faulted;
}
static void leo_mdp_inc(volatile uint32_t *value)
{ if(*value!=UINT32_MAX)++*value; }
static void leo_mdp_first_failure(uint32_t phase,uint64_t begin,
                                  uint64_t interval,uint32_t polls)
{
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 if(!leo_mdp_fault_state.first_valid){
  uint64_t now=begin?mach_absolute_time():0;
  leo_mdp_fault_state.first_phase=phase;
  leo_mdp_fault_state.first_status=leo_mdp?leo_read(0x24):UINT32_MAX;
  leo_mdp_fault_state.first_config=leo_mdp?leo_read(0x90000):UINT32_MAX;
  leo_mdp_fault_state.first_size=leo_mdp?leo_read(0x90004):UINT32_MAX;
  leo_mdp_fault_state.first_address=leo_mdp?leo_read(0x90008):UINT32_MAX;
  leo_mdp_fault_state.first_stride=leo_mdp?leo_read(0x9000c):UINT32_MAX;
  leo_mdp_fault_state.first_xy=leo_mdp?leo_read(0x90010):UINT32_MAX;
  leo_mdp_fault_state.first_polls=polls;
  leo_mdp_fault_state.first_pending=leo_graphics_late_pending;
  leo_mdp_fault_state.first_attempts=leo_obs_attempts;
  leo_mdp_fault_state.first_kicks=leo_obs_graphics_kicks;
  leo_mdp_fault_state.first_completed=leo_obs_completed;
  leo_mdp_fault_state.first_elapsed=begin&&now>=begin?now-begin:0;
  leo_mdp_fault_state.first_interval=interval;
  leo_mdp_fault_state.first_sequence=leo_graphics_sequence;
  leo_sync();leo_mdp_fault_state.first_valid=1;
 }
 ml_set_interrupts_enabled(enabled);
}
static int leo_graphics_failed(uint32_t phase,uint64_t begin,
                                uint64_t interval,uint32_t polls)
{
 int result=leo_obs_failed(phase);
 leo_mdp_first_failure(phase,begin,interval,polls);return result;
}
/* Normal serialized graphics worker only. The short IRQ mask prevents the
 * fatal painter from interleaving validation, acknowledgement and publication.
 * Return0 means no actual DONE; negative values retain the original phase. */
static int leo_graphics_try_complete(uint64_t *completed_sequence,int deadline_sample)
{
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 int result=0;
 if(leo_fatal_active)result=-7;
 else if(leo_graphics_faulted)result=-1;
 else {
  leo_sync();unsigned int status=leo_read(0x24);leo_sync();
  if(status&(1U<<14)){
   if(!leo_dma_ok()){leo_graphics_faulted=1;result=-8;}
   else {
    leo_write(0x28,1U<<14);leo_sync();
    if(leo_read(0x24)&(1U<<14)){leo_graphics_faulted=1;result=-6;}
    else {
     leo_refresh_pending=0;
     ++leo_graphics_sequence;if(!leo_graphics_sequence)++leo_graphics_sequence;
     ++leo_obs_completed;*completed_sequence=leo_graphics_sequence;result=1;
    }
   }
  }else if(deadline_sample){
   if(!leo_dma_ok()){leo_graphics_faulted=1;result=-8;}
   else {leo_graphics_late_pending=1;leo_mdp_inc(&leo_mdp_fault_state.late_pending_count);}
  }
 }
 ml_set_interrupts_enabled(enabled);return result;
}
/* Late DONE only retires an old physical request. It never increments logical
 * completion/sequence or writes the caller's completed_sequence output. */
static int leo_graphics_retire_late(void)
{
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 int result=1;
 if(leo_fatal_active)result=-7;
 else if(leo_graphics_faulted)result=-1;
 else if(!ios7leo_display_ready()){leo_graphics_faulted=1;result=-11;}
 else {
  leo_sync();unsigned int status=leo_read(0x24);leo_sync();
  if(!(status&(1U<<14))){leo_mdp_inc(&leo_mdp_fault_state.pending_rejects);result=-10;}
  else {
   leo_write(0x28,1U<<14);leo_sync();
   if(leo_read(0x24)&(1U<<14)){leo_graphics_faulted=1;result=-12;}
   else {
    leo_refresh_pending=0;leo_graphics_late_pending=0;
    leo_mdp_inc(&leo_mdp_fault_state.late_ack_count);
    leo_mdp_fault_state.last_ack_status=status;
    if(!leo_mdp_fault_state.first_recovery_valid){
     leo_mdp_fault_state.first_recovery_status=status;
     leo_mdp_fault_state.first_recovery_pending_count=leo_mdp_fault_state.late_pending_count;
     leo_mdp_fault_state.first_recovery_kicks=leo_obs_graphics_kicks;
     leo_mdp_fault_state.first_recovery_completed=leo_obs_completed;
     leo_mdp_fault_state.first_recovery_sequence=leo_graphics_sequence;
     leo_sync();leo_mdp_fault_state.first_recovery_valid=1;
    }
   }
  }
 }
 ml_set_interrupts_enabled(enabled);return result;
}
void ios7leo_mdp_observe(LeoMDPFaultObservation *out)
{
 if(!out || out->version!=IOS7_LEO_MDP_OBS_VERSION)return;
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 *out=*(const LeoMDPFaultObservation *)&leo_mdp_fault_state;
 out->version=IOS7_LEO_MDP_OBS_VERSION;out->known=LEO_MDP_KNOWN_LIVE;
 if(out->first_valid)out->known|=LEO_MDP_KNOWN_FIRST_FAILURE;
 if(out->first_recovery_valid)out->known|=LEO_MDP_KNOWN_FIRST_RECOVERY;
 out->pending=leo_graphics_late_pending;out->latched=leo_graphics_faulted;
 ml_set_interrupts_enabled(enabled);
}
int ios7leo_display_present(const uint8_t *pixels,uint32_t bytes,uint32_t row,
                           uint32_t format,uint64_t *completed_sequence)
{
 ++leo_obs_attempts;leo_obs_last_rows=0;
 /* Bad caller geometry does not retire or replace a pending physical request. */
 if(!pixels || !completed_sequence || !leo_scanout_geometry(bytes,row,format))return leo_obs_failed(1);
 if(leo_graphics_late_pending){
  int retired=leo_graphics_retire_late();
  if(retired<0)return leo_graphics_failed((uint32_t)-retired,0,0,0);
 }
 boolean_t ready_enabled=ml_set_interrupts_enabled(FALSE);
 int ready=ios7leo_display_ready();
 /* With mappings present and no prior fatal/latch, a failed ready predicate
  * is a video/DMA configuration failure, not a recoverable timeout. */
 if(!ready && leo_pixels && leo_mdp && !leo_fatal_active && !leo_graphics_faulted)
  leo_graphics_faulted=1;
 ml_set_interrupts_enabled(ready_enabled);
 if(!ready)return leo_graphics_failed(1,0,0,0);
 /* Caller serializes presentation. This single-CPU board disables interrupts
  * for one bounded480-pixel row so a fatal painter cannot interleave that row.
  * No locks/heap/console output/SD I/O during row conversion or completion poll. */
 if(!leo_graphics_owner){
  boolean_t enabled=ml_set_interrupts_enabled(FALSE);
  leo_obs_normal_at_claim=leo_obs_normal_kicks;
  (void)switch_to_serial_console();leo_graphics_owner=1;
  ml_set_interrupts_enabled(enabled);
 }
 if(leo_fatal_active)return leo_graphics_failed(2,0,0,0);
 if(!leo_drain_refresh()){leo_graphics_faulted=1;return leo_graphics_failed(3,0,0,0);}
 for(unsigned int y=0;y<800;y++){
  boolean_t enabled=ml_set_interrupts_enabled(FALSE);
  if(leo_fatal_active){ml_set_interrupts_enabled(enabled);return leo_graphics_failed(4,0,0,0);}
  const uint8_t *source=pixels+(uint64_t)y*row;
  leo_scanout_copy_row(leo_pixels+y*480,source,format);
  ++leo_obs_rows;++leo_obs_last_rows;
  ml_set_interrupts_enabled(enabled);
 }
 /* MDP31 (HD2 defconfig), not MDP40. Clear only raw DMA_P_DONE bit14;
  * require a new hardware completion after this request's real kick. */
 boolean_t kick_enabled=ml_set_interrupts_enabled(FALSE);
 if(leo_fatal_active){ml_set_interrupts_enabled(kick_enabled);return leo_graphics_failed(5,0,0,0);}
 if(!leo_dma_ok()){leo_graphics_faulted=1;ml_set_interrupts_enabled(kick_enabled);return leo_graphics_failed(5,0,0,0);}
 leo_sync();leo_write(0x28,1U<<14);leo_sync();
 if(leo_read(0x24)&(1U<<14)){leo_graphics_faulted=1;ml_set_interrupts_enabled(kick_enabled);return leo_graphics_failed(6,0,0,0);}
 leo_write(0x44,0U);leo_sync();leo_refresh_pending=1;
 ++leo_obs_graphics_kicks;
 ml_set_interrupts_enabled(kick_enabled);
 uint64_t begin=mach_absolute_time(),interval=0;
 (nanoseconds_to_absolutetime)(100000000ULL,&interval);
 unsigned int polls=0;
 for(;polls<1000000U;){
  ++polls;int finished=leo_graphics_try_complete(completed_sequence,0);
  if(finished>0)return 1;
  if(finished<0)return leo_graphics_failed((uint32_t)-finished,begin,interval,polls);
  if(mach_absolute_time()-begin>=interval)break;
 }
 /* A worker can be preempted between its no-DONE read and deadline check.
  * Resample on BOTH deadline and poll-limit exits; only actual DONE succeeds. */
 int finished=leo_graphics_try_complete(completed_sequence,1);
 if(finished>0)return 1;
 if(finished<0)return leo_graphics_failed((uint32_t)-finished,begin,interval,polls+1U);
 /* Retain one physical request without repainting/re-kicking it. A future
  * call may retire real late DONE, but this failed call owns no completion. */
 return leo_graphics_failed(9,begin,interval,polls+1U);
}

void ios7leo_scanout_observe(LeoDisplayObservation *out)
{
 if(!out || out->version!=1)return;
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 out->known|=LEO_OBS_PE_COUNTERS;
 out->present_attempts=leo_obs_attempts;out->conversion_rows=leo_obs_rows;
 out->last_conversion_rows=leo_obs_last_rows;out->completed=leo_obs_completed;
 out->failures=leo_obs_failures;out->last_failure=leo_obs_failure_phase;
 out->graphics_owner=leo_graphics_owner;out->fatal_active=leo_fatal_active;
 out->normal_refresh_requests=leo_obs_refresh_requests;out->normal_owner_blocks=leo_obs_owner_blocks;
 out->normal_pending_skips=leo_obs_pending_skips;out->normal_kicks=leo_obs_normal_kicks;
 out->normal_kicks_at_graphics_claim=leo_obs_normal_at_claim;
 out->normal_kicks_after_owner=leo_obs_normal_after_owner;
 out->graphics_kicks=leo_obs_graphics_kicks;out->completed_sequence_before=leo_graphics_sequence;
 ml_set_interrupts_enabled(enabled);
 /* Normal worker only. No IRQ masking across the bounded1024 pixel reads. */
 if(leo_pixels && leo_mdp && PE_state.video.v_width==480 && PE_state.video.v_height==800 &&
    PE_state.video.v_rowBytes==960 && PE_state.video.v_depth==16){
  uint32_t hash=2166136261U,nonzero=0;
  for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
   unsigned index=((y*799U)/31U)*480U+(x*479U)/31U;
   uint32_t value=leo_pixels[index];hash=(hash^value)*16777619U;
   if(value)++nonzero;
  }
  out->scanout_sample_hash=hash;out->scanout_nonzero_rgb=nonzero;out->scanout_samples=1024;
  out->last_status=leo_read(0x24);out->known|=LEO_OBS_SCANOUT_SAMPLES;
 }
 enabled=ml_set_interrupts_enabled(FALSE);
 out->completed_sequence_after=leo_graphics_sequence;
 ml_set_interrupts_enabled(enabled);
}

void ios7leo_checkpoint(boot_args *args, unsigned int stage)
{
 if(leo_fatal_active)return;
 if(stage==6) {
  if(!leo_video_ok(args)) ios7leo_final_boundary(1);
  leo_fb_physical=args->Video.v_baseAddr;
  /* locore provides these limited device identity mappings before MMU on. */
  leo_pixels=(volatile unsigned short *)leo_fb_physical;
  leo_mdp=IOS7LAB_LEO_MDP_PHYSICAL;
  if(!leo_dma_ok()) {leo_pixels=0;ios7leo_final_boundary(2);}
 }
 if(stage!=leo_stage+1U || stage<6 || stage>10) {
  /* First stage follows loader5, not an implied successful previousXNUstage. */
  if(!(stage==6 && leo_stage==0)) ios7leo_final_boundary(3);
 }
 leo_stage=stage;
 leo_render(leo_callsite(__builtin_return_address(0)),IOS7LAB_LEO_STAGE_BASE|stage);
}
void ios7leo_forget_early_maps(void)
{ leo_pixels=0;leo_mdp=0; }
void ios7leo_debug_putc(char c)
{ if(leo_console_count<sizeof(leo_console)) leo_console[leo_console_count++]=c; }
static int leo_getc(void) { return -1; }
static void leo_uart_init(void) { /* no UART success claim */ }

void PE_init_SocSupport_stub(void)
{
 DTEntry chosen;
 ios7lab_leo_handoff *h=0;
 unsigned int size=0;
 boot_args *args=(boot_args *)PE_state.bootArgs;
 vm_offset_t fb;
 if(!leo_video_ok(args) || DTLookupEntry(0,"/chosen",&chosen)!=kSuccess ||
    DTGetProperty(chosen,IOS7LAB_LEO_DT_PROPERTY,(void **)&h,&size)!=kSuccess ||
    size!=128 || !h || h->magic!=IOS7LAB_LEO_MAGIC || h->version!=1 ||
    h->bytes!=128 || h->flags!=3 || h->machine!=2524 ||
    (h->entry_sctlr & IOS7LAB_LEO_SCTLR_REJECT) ||
    (h->entry_cpsr & 0x23fU)!=0x13U ||
    h->mdp_physical!=IOS7LAB_LEO_MDP_PHYSICAL ||
    h->dma_address!=args->Video.v_baseAddr || h->dma_size!=((800U<<16)|480U) ||
    h->dma_stride!=960 || h->dma_xy!=0 ||
    (h->dma_config&0x06003f80U)!=0x02002100U ||
    h->width!=480 || h->height!=800 || h->row_bytes!=960 ||
    h->pixel_format!=IOS7LAB_LEO_RGB565 || h->framebuffer_bytes!=768000 ||
    h->memory_base!=args->physBase || h->memory_bytes!=args->memSize ||
    h->top_of_kernel_data!=args->topOfKernelData || h->checkpoint_count!=5 ||
    h->reserved0 || h->reserved1) ios7leo_final_boundary(4);
 /* RAM witness is independent of graphics success: a map/parse error remains
  * explicitly unknown while the normal PE path continues unchanged. */
 (void)leo_memory_capture_functional(h);
 /* Standard ml_io_map passes VM_WIMG_IO; physical DMA and CPU VA distinct. */
 fb=ml_io_map(leo_fb_physical,768000);
 leo_mdp=ml_io_map(IOS7LAB_LEO_MDP_PHYSICAL,IOS7LAB_LEO_MDP_MAP_BYTES);
 if(!fb || !leo_mdp) ios7leo_final_boundary(5);
 leo_pixels=(volatile unsigned short *)fb;
 if(!leo_dma_ok()) {leo_pixels=0;ios7leo_final_boundary(6);}
 PE_state.video.v_baseAddr=fb;
 PE_state.video.v_rowBytes=960;PE_state.video.v_width=480;
 PE_state.video.v_height=800;PE_state.video.v_depth=16;

 /* Only bounded RAM text, not another implicit video/UART diagnostic writer. */
 gPESocDispatch.uart_putc=ios7leo_debug_putc;
 gPESocDispatch.uart_getc=leo_getc;
 gPESocDispatch.uart_init=leo_uart_init;
 gPESocDispatch.interrupt_init=Leo_interrupt_init;
 gPESocDispatch.timebase_init=Leo_timebase_init;
 gPESocDispatch.handle_interrupt=Leo_handle_interrupt;
 gPESocDispatch.get_timebase=Leo_get_timebase;
 gPESocDispatch.timer_value=Leo_timer_value;
 gPESocDispatch.timer_enabled=Leo_timer_enabled;
 {
  /* Genericconsole owns all native480x800 pixels; no normal overlay writer.
   * PE(TRUE) precedes kmem_init: tag knownphysical even beforekernel_map. */
  PE_Video console_video=PE_state.video;
  console_video.v_baseAddr=leo_fb_physical|1U;
  console_video.v_baseAddrHigh=0;
  console_video.v_offset=0;console_video.v_length=800U*960U;
  console_video.v_width=480;console_video.v_height=800;
  console_video.v_rowBytes=960;console_video.v_depth=16;
  console_video.v_scale=kPEScaleFactor1x;
  initialize_screen(&console_video,kPETextMode);
  DTEntry chosen;void *fast_property=0;unsigned int fast_bytes=0;
  if(DTLookupEntry(NULL,"/chosen",&chosen)==kSuccess &&
     DTGetProperty(chosen,"ios7leo-fast-console",&fast_property,&fast_bytes)==kSuccess &&
     fast_property && fast_bytes==4 &&
     ((const unsigned char *)fast_property)[0]==1 &&
     ((const unsigned char *)fast_property)[1]==0 &&
     ((const unsigned char *)fast_property)[2]==0 &&
     ((const unsigned char *)fast_property)[3]==0){
   printf("Leo FASTBOOT console: real native console initialized; ordinary output to retained RAM sink\n");
   (void)switch_to_serial_console();
  }

 }
 ios7leo_checkpoint(args,9);
 __asm__ volatile("" ::: "memory"); /* keep checkpoint return before PE return */
 /* The original PE path now initializes real VIC; clock_init owns GPT init. */
}
#endif
