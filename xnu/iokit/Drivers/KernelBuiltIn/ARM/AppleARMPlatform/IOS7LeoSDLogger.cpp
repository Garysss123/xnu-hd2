#if BOARD_CONFIG_QSD8250_LEO
#include <IOKit/IOService.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/OSAtomic.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
extern "C" {extern unsigned int vm_page_free_count,vm_page_free_reserved,vm_page_free_target;}
/* Exact private kernel C API: kern/thread.h480, thread.c316. */
extern "C" void thread_terminate_self(void);
#include "IOS7LeoSDLog.h"
#include "IOS7LeoSDLogNonce.h"
#include "IOS7LeoSDLogIdentity.h"
extern "C" {
#include "leo_log_format.h"
#include "leo_log_locator.h"
}
#include "leo_user_frontier.h"
#include "IOS7LeoDisplayObservation.h"
#include "IOS7LeoGraphicsState.h"
#include "IOS7LeoMemoryObservation.h"
#include "IOS7LeoMDPFault.h"
#include "IOS7LeoTouchObservation.h"
extern "C" {
#include "../../../../../osfmk/arm/ios7lab_fault_witness_shared.h"
}
static uint8_t logBytes[LEO_LOG_SPOOL_BYTES];
static leo_log_spool spool={logBytes,LEO_LOG_SPOOL_BYTES,0,0,0};
static volatile UInt32 spoolGuard,busyDrops,unknownDrops;
static uint64_t log_now_ns(void){uint64_t value=0;(absolutetime_to_nanoseconds)(mach_absolute_time(),&value);return value;}
/* Try-only producer: no blocking lock/clock/heap/SD/scheduling/printf. */
extern "C" void ios7leo_log_putc(char c)
{
 if(!OSCompareAndSwap(0,1,&spoolGuard)){
  for(unsigned n=0;n<4;n++){UInt32 value=busyDrops;if(value==0xffffffffU)return;if(OSCompareAndSwap(value,value+1,&busyDrops))return;}
  (void)OSCompareAndSwap(0,1,&unknownDrops);return;
 }
 leo_log_spool_put(&spool,(uint8_t)c);OSMemoryBarrier();spoolGuard=0;
}
class IOS7LeoSDLogger:public IOService {
 OSDeclareDefaultStructors(IOS7LeoSDLogger)
 IOMedia *raw;IOService *device;IOBufferMemoryDescriptor *bounce;
 fl32_work *work;fl32_extent *ext;leo_log_dir *dirs;uint8_t *owners;
 ios7leo_sd_log_extent *ranges;
 fl32_image image;ios7leo_sd_log_capability *cap;ios7leo_sd_log_info info;
 bool opened;uint32_t next;uint64_t startNs;int result;
 static int read512(void *,uint64_t,uint8_t[512]);
 static void worker(void *,wait_result_t);
 IOReturn readSector(uint64_t,uint8_t[512]);
 bool prepare();void run();void releaseResources();
 bool appendObservation(uint8_t *,uint32_t,uint8_t[512]);
 bool observe(uint32_t,uint64_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeActivity(uint32_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeExtended(uint32_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeMDP(bool &,bool &,uint8_t *,uint8_t[512]);
 bool observeTouch(bool[5],uint8_t *,uint8_t[512]);
 enum {Pending=1,Failed=2,Active=3,Full=4};volatile uint32_t loggerState;
public:
 virtual bool init(OSDictionary *p=0);
 virtual IOService *probe(IOService *,SInt32 *);
 virtual bool start(IOService *);
 virtual void free();
};
OSDefineMetaClassAndStructors(IOS7LeoSDLogger,IOService)
bool IOS7LeoSDLogger::init(OSDictionary *p)
{raw=0;device=0;bounce=0;work=0;ext=0;dirs=0;owners=0;ranges=0;cap=0;opened=false;next=1;startNs=0;result=0;loggerState=Pending;bzero(&image,sizeof(image));bzero(&info,sizeof(info));return IOService::init(p);}
IOService *IOS7LeoSDLogger::probe(IOService *provider,SInt32 *)
{
 IOMedia *m=OSDynamicCast(IOMedia,provider);if(!m||!m->isWhole())return 0;
 IOBlockStorageDriver *d=OSDynamicCast(IOBlockStorageDriver,m->getProvider());IOService *s=d?d->getProvider():0;
 return s&&s->metaCast("IOS7LeoSDCC2")?this:0;
}
IOReturn IOS7LeoSDLogger::readSector(uint64_t lba,uint8_t bytes[512])
{
 if(!opened||!raw||!bounce||lba>=info.card_sectors||lba>UINT64_MAX/512U)return kIOReturnBadArgument;
 UInt64 actual=0;IOReturn r=raw->read(this,lba*512U,bounce,&actual);
 if(r==kIOReturnSuccess&&(actual!=512||bounce->readBytes(0,bytes,512)!=512))r=kIOReturnUnderrun;
 return r;
}
int IOS7LeoSDLogger::read512(void *p,uint64_t lba,uint8_t out[512]){return (int)((IOS7LeoSDLogger *)p)->readSector(lba,out);}
bool IOS7LeoSDLogger::prepare()
{
 result=ios7leo_sd_log_describe(device,&info);if(result)return false;
 if(!info.initialized||info.faulted){result=(int)kIOReturnNotReady;return false;}
 if(info.csd_write_protected){result=(int)kIOReturnNotWritable;return false;}
 if(!info.card_sectors||raw->getPreferredBlockSize()!=512||raw->getSize()/512U!=info.card_sectors||(raw->getSize()&511U)){result=(int)kIOReturnBadArgument;return false;}
 /* Rounded heap/bounce + alreadyresident spool + conservatively charged private
  * cap page; reserve the same managed-quarter headroom as the COW device. */
 uint64_t managed=sane_size>>12;
 uint64_t charge=((sizeof(*work)+4095U)>>12)+((128U*sizeof(*ext)+4095U)>>12)+
  ((LEO_LOG_DIR_CAP*sizeof(*dirs)+4095U)>>12)+((((LEO_LOG_CLUSTER_CAP+9U)/8U)+4095U)>>12)+
  ((128U*sizeof(*ranges)+4095U)>>12)+((sizeof(*this)+4095U)>>12)+1U+4U+1U;
 uint64_t guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 uint64_t freePages=vm_page_free_count;
 if(!managed||charge>(managed>>3)||freePages<guard||charge>(freePages-guard)/2U){result=(int)kIOReturnNoMemory;return false;}
 bounce=IOBufferMemoryDescriptor::withOptions(kIODirectionIn,512,4);
 work=(fl32_work *)IOMalloc(sizeof(*work));ext=(fl32_extent *)IOMalloc(128U*sizeof(*ext));
 dirs=(leo_log_dir *)IOMalloc(LEO_LOG_DIR_CAP*sizeof(*dirs));owners=(uint8_t *)IOMalloc((LEO_LOG_CLUSTER_CAP+9U)/8U);
 ranges=(ios7leo_sd_log_extent *)IOMalloc(128U*sizeof(*ranges));
 if(!bounce||!work||!ext||!dirs||!owners||!ranges){result=(int)kIOReturnNoMemory;return false;}
 guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 if((uint64_t)vm_page_free_count<guard){result=(int)kIOReturnNoMemory;return false;}
 fl32_io io={this,read512,info.card_sectors};fl32_limits limits={262144U,128U,LEO_LOG_BYTES};
 result=leo_log_locate(&io,&limits,work,ext,128,&image,ios7leo_log_nonce,owners,(LEO_LOG_CLUSTER_CAP+9U)/8U,dirs,LEO_LOG_DIR_CAP);
 if(result)return false;
 uint8_t sector[512];next=1;
 for(uint32_t slot=1;slot<LEO_LOG_SECTORS;slot++){
  uint64_t physical;uint32_t run;result=leo_log_translate(&image,ext,128,slot,&physical,&run);if(result)return false;
  result=(int)readSector(physical,sector);if(result)return false;
  unsigned occupied=0;for(unsigned b=0;b<512;b++)occupied|=sector[b];if(occupied)next=slot+1;
 }
 for(uint32_t i=0;i<image.extent_count;i++)ranges[i]=(ios7leo_sd_log_extent){ext[i].logical_lba,ext[i].physical_lba,ext[i].sectors};
 result=ios7leo_sd_log_bind(device,ranges,image.extent_count,next,ios7leo_log_nonce,&cap);
 return result==0&&cap;
}
bool IOS7LeoSDLogger::start(IOService *provider)
{
 if(!IOService::start(provider))return false;
 raw=OSDynamicCast(IOMedia,provider);if(!raw)return false;raw->retain();
 IOBlockStorageDriver *d=OSDynamicCast(IOBlockStorageDriver,raw->getProvider());device=d?d->getProvider():0;
 if(!device||!device->metaCast("IOS7LeoSDCC2")){device=0;return false;}device->retain();
 opened=raw->open(this,0,kIOStorageAccessReader);if(!opened)return false;
 startNs=log_now_ns();loggerState=Pending;
 IOLog("Leo log PENDING: worker preparation requested; raw shared-reader open=1\n");
 retain();thread_t thread=THREAD_NULL;kern_return_t kr=kernel_thread_start(worker,this,&thread);
 if(kr!=KERN_SUCCESS){loggerState=Failed;release();IOLog("Leo log FAILED: worker creation=%d\n",kr);return false;}
 thread_deallocate(thread);return true;
}
void IOS7LeoSDLogger::worker(void *p,wait_result_t)
{
 IOS7LeoSDLogger *self=(IOS7LeoSDLogger *)p;
 if(!self->prepare()){
  self->loggerState=Failed;
  IOLog("Leo log FAILED: prepare result=%d (no write; rootpolicy unchanged)\n",self->result);
  self->releaseResources();
 }else{self->releaseResources();self->run();}
 self->release();thread_terminate_self();
}
bool IOS7LeoSDLogger::appendObservation(uint8_t *payload,uint32_t bytes,uint8_t sector[512])
{
 if(next>=LEO_LOG_SECTORS){loggerState=Full;return false;}
 result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,0,payload,bytes);
 if(!result)result=ios7leo_sd_log_write(cap,next,sector);
 if(result){loggerState=Failed;IOLog("Leo observation write failed=%d next=%u no retry/sharedfault preserved\n",result,(unsigned)next);return false;}
 next++;return true;
}
bool IOS7LeoSDLogger::observe(uint32_t milestone,uint64_t requested,uint64_t now,uint8_t *payload,uint8_t sector[512])
{
 LeoDisplayObservation d;bzero(&d,sizeof(d));d.version=1;
 ios7leo_scanout_observe(&d);ios7leo_primary_observe(&d);
 ios7leo_service_activity a;bzero(&a,sizeof(a));uint32_t activityKnown=ios7leo_frontier_activity(&a);
 uint32_t clockValid=now>=startNs;uint64_t elapsed=clockValid?now-startNs:0;
 uint64_t deadline=requested*1000000000ULL;uint64_t late=clockValid && elapsed>=deadline?elapsed-deadline:0;
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS M=%u PART=1 REQ=%llu NS=%llu CLOCK_OK=%u ELAPSED=%llu LATE=%llu KNOWN=%x OWN=%u FATAL=%u ATT=%u ROWS=%u LROWS=%u DONE=%u FAIL=%u FERR=%x NREQ=%u BLOCK=%u PSKIP=%u NK=%u GK=%u KCLAIM=%u KAFTER=%u STATUS=%x kick!=DONE\n",milestone,(unsigned long long)requested,(unsigned long long)now,clockValid,(unsigned long long)elapsed,(unsigned long long)late,d.known,d.graphics_owner,d.fatal_active,d.present_attempts,d.conversion_rows,d.last_conversion_rows,d.completed,d.failures,d.last_failure,d.normal_refresh_requests,d.normal_owner_blocks,d.normal_pending_skips,d.normal_kicks,d.graphics_kicks,d.normal_kicks_at_graphics_claim,d.normal_kicks_after_owner,d.last_status);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS M=%u PART=2 NOW_NS=%llu KNOWN=%x SEQ=%llu:%llu SCAN_HASH=%08x SCAN_RGB=%u SCAN_N=%u PRIMARY_HASH=%08x PRIMARY_RGB=%u PRIMARY_ALPHA=%u PRIMARY_N=%u BYTES=%u SEED=%u:%u SURFACE=%u 1024-grid-only;metadata-stability-not-atomic-user-frame\n",milestone,(unsigned long long)now,d.known,(unsigned long long)d.completed_sequence_before,(unsigned long long)d.completed_sequence_after,d.scanout_sample_hash,d.scanout_nonzero_rgb,d.scanout_samples,d.primary_sample_hash,d.primary_nonzero_rgb,d.primary_nonzero_alpha,d.primary_samples,d.primary_bytes,d.primary_seed_before,d.primary_seed_after,d.active_surface);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS M=%u PART=3 NOW_NS=%llu SERVICE_KNOWN=%u PID=%u TID=%llu REQ=%u REP=%u LAST_REQ=%u LAST_REP=%u STATUS_KIND=%u STATUS=%x DISPOSITION=%u SAT=%u PAIRS=%u LIMIT=%u LATE_WAIT=%u LOSS=%u UNK=%u LAST_TICK_ABS=%llu same-target-only;tryguard-loss-qualified\n",milestone,(unsigned long long)now,activityKnown,a.pid,(unsigned long long)a.tid,a.requests,a.replies,a.last_request,a.last_reply,a.status_kind,a.status,a.disposition,a.saturated,a.service_pairs,a.limit_reached,a.late_wait,a.loss,a.unknown,(unsigned long long)a.last_tick);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 if(!observeActivity(milestone,now,payload,sector))return false;
 return observeExtended(milestone,now,payload,sector);
}
/* Normal worker only. IRQ snapshot restores its entry mask before formatting,
 * pixel observation or storage access. Counts are samples, not CPU percent. */
bool IOS7LeoSDLogger::observeActivity(uint32_t milestone,uint64_t now,uint8_t *payload,uint8_t sector[512])
{
 ios7lab_irq_summary sample;bzero(&sample,sizeof(sample));
 uint32_t known=ios7lab_irq_snapshot(&sample);
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTACT M=%u PART=0 NOW_NS=%llu KNOWN=%u USER_IRQ=%u KERNEL_IRQ=%u UNKNOWN_IRQ=%u SAT=%u SB_RING_HEAD=%u SB_RING_OVERWRITTEN=%u IRQ-samples-not-CPU-percent;role-counts-all-threads;tuples-last-observed\n",milestone,(unsigned long long)now,known,sample.user_irqs,sample.kernel_irqs,sample.unknown_irqs,sample.saturated,sample.ring_head,sample.ring_loss);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 for(uint32_t start=0;start<IOS7LAB_FAULT_WITNESS_IRQ_JOBS+1U;start+=3U){
  n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTACT M=%u PART=%u ROLE:SEEN,PID,TID,COUNT,PC,SP,LR,CPSR ",milestone,1U+start/3U);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  uint32_t used=(uint32_t)n;
  for(uint32_t i=start;i<start+3U&&i<IOS7LAB_FAULT_WITNESS_IRQ_JOBS+1U;i++){
   const ios7lab_irq_job_snapshot *j=&sample.jobs[i];
   n=snprintf((char *)payload+used,LEO_LOG_PAYLOAD-used,"[%u:%u,%u,%llu,%u,%08x,%08x,%08x,%08x] ",i,(unsigned)(j->count!=0),j->pid,(unsigned long long)j->tid,j->count,j->last_pc,j->last_sp,j->last_lr,j->last_cpsr);
   if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD-used){result=kIOReturnNoSpace;loggerState=Failed;return false;}
   used+=(uint32_t)n;
  }
  if(used+1U>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  payload[used++]='\n';payload[used]=0;
  if(!appendObservation(payload,used,sector))return false;
 }
 return true;
}
bool IOS7LeoSDLogger::observeExtended(uint32_t milestone,uint64_t now,uint8_t *payload,uint8_t sector[512])
{
 LeoGraphicsState g;bzero(&g,sizeof(g));g.version=LEO_GRAPHICS_STATE_VERSION;
 uint32_t known=ios7leo_graphics_snapshot(&g);
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=1 KNOWN=%u:%u LOSS=%u UNK=%u CID=%x PID=%u S4=%u/%u S5=%u/%u/%u/%u S6=%u/%u S9=%u/%u PORT=%u/%u SEND=%u/%u/%u COPYFAIL=%u STALE=%u\n",milestone,known,g.known,g.loss,g.unknown,g.last_client,g.owner_pid,g.selector4_calls,g.selector4_rejects,g.selector5_calls,g.selector5_rejects,g.selector5_backend_calls,g.selector5_completed,g.selector6_calls,g.selector6_rejects,g.selector9_calls,g.selector9_rejects,g.port_calls,g.port_rejects,g.send_calls,g.send_success,g.send_failures,g.copy_send_failures,g.stale_completions);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=2 KNOWN=%u CID=%x PID=%u LAST=%u/%x/%x TX=%x:%x SURFACE=%u MM=%x WAIT=%x:%x CFG=%u/%u/%u/%u/%u GEN=%x:%x SENDSTAT=%x TIMERSTAT=%x EVENTS=%u:%u aggregate-last-client;not-physical-vsync\n",milestone,known,g.last_client,g.owner_pid,g.last_selector,g.last_result,g.last_reason,g.last_transaction_hi,g.last_transaction_lo,g.last_surface,g.last_mismatch,g.last_wait_hi,g.last_wait_lo,g.requested,g.enabled,g.closed,g.port_present,g.callback_present,g.generation_hi,g.generation_lo,g.last_send_status,g.last_timer_status,g.events_published,g.events_taken);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=3 KNOWN=%u RESULT4=%x REASON4=%x RESULT5=%x REASON5=%x RESULT6=%x REASON6=%x RESULT9=%x REASON9=%x failures-before-PE-are-included\n",milestone,known,g.selector4_result,g.selector4_reason,g.selector5_result,g.selector5_reason,g.selector6_result,g.selector6_reason,g.selector9_result,g.selector9_reason);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=4 OPEN=%x/%u Q3=%u/%u Q8=%u/%u Q18=%u/%u QLAST=%x/%u/%u/%x PIPE=%u/%u/%u/%u/%u PENTRY=%x/%u/%u/%u RET=%x historical-identities\n",milestone,g.latest_open_client,g.latest_open_pid,g.selector3_calls,g.selector3_successes,g.selector8_calls,g.selector8_successes,g.selector18_calls,g.selector18_successes,g.prereq_last_client,g.prereq_last_pid,g.prereq_last_selector,g.prereq_last_result,g.port_entries,g.port_preflight_rejects,g.port_gate_entries,g.port_calls,g.port_returns,g.port_entry_client,g.port_entry_pid,g.port_entry_type,g.port_entry_valid,g.port_last_return);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 LeoMemoryObservation v;bzero(&v,sizeof(v));v.version=IOS7_LEO_MEMORY_OBS_VERSION;
 ios7leo_memory_observe(&v);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTMEM M=%u NS=%llu KNOWN=%x STATUS=%u SEQ=%u SELECTED=%08x:%08x MANAGED=%llu FREE=%u RESERVE=%u TARGET=%u PAGEINS=%llu PAGEOUTS=%llu FAULTS=%llu BANKS=%u ADMIT=%u sampled-master-counters-not-process-cost\n",milestone,(unsigned long long)now,v.known,v.status,v.sequence,v.selected_base,v.selected_bytes,(unsigned long long)v.managed_bytes,v.free_pages,v.free_reserved,v.free_target,(unsigned long long)v.pageins,(unsigned long long)v.pageouts,(unsigned long long)v.faults,v.bank_count,(unsigned)((v.known&LEO_MEMORY_KNOWN_BOARD_PROFILE)!=0));
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 if(milestone==0)for(uint32_t start=0;start<IOS7_LEO_MEMORY_MAX_BANKS;start+=4U){
  n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTBANK PART=%u KNOWN=%x STATUS=%u COUNT=%u ATAG=%08x:%08x SELECTED=%08x:%08x ",1U+start/4U,v.known,v.status,v.bank_count,v.atag_base,v.atag_end,v.selected_base,v.selected_bytes);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  uint32_t used=(uint32_t)n;
  for(uint32_t i=start;i<start+4U;i++){
   n=snprintf((char *)payload+used,LEO_LOG_PAYLOAD-used,"B%u=%08x:%08x ",i,v.banks[i].base,v.banks[i].bytes);
   if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD-used){result=kIOReturnNoSpace;loggerState=Failed;return false;}used+=(uint32_t)n;
  }
  if(used+1U>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  payload[used++]='\n';payload[used]=0;if(!appendObservation(payload,used,sector))return false;
 }
 return true;
}
/* Two one-shot records in the existing worker. Snapshot only reads RAM;
 * record writes occur after the accessor restores its incoming IRQ mask. */
bool IOS7LeoSDLogger::observeMDP(bool &firstLogged,bool &recoveryLogged,
    uint8_t *payload,uint8_t sector[512])
{
 LeoMDPFaultObservation d;bzero(&d,sizeof(d));d.version=IOS7_LEO_MDP_OBS_VERSION;
 ios7leo_mdp_observe(&d);
 if(!firstLogged && d.first_valid && (d.known&LEO_MDP_KNOWN_FIRST_FAILURE)){
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTMDP FIRST PHASE=%u STATUS=%08x CFG=%08x SIZE=%08x ADDR=%08x STRIDE=%u XY=%08x POLLS=%u PENDING=%u ATT=%u KICK=%u DONE=%u ELAPSED_ABS=%llu LIMIT_ABS=%llu SEQ=%llu\n",d.first_phase,d.first_status,d.first_config,d.first_size,d.first_address,d.first_stride,d.first_xy,d.first_polls,d.first_pending,d.first_attempts,d.first_kicks,d.first_completed,(unsigned long long)d.first_elapsed,(unsigned long long)d.first_interval,(unsigned long long)d.first_sequence);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  firstLogged=true;
 }
 if(!recoveryLogged && d.first_recovery_valid && (d.known&LEO_MDP_KNOWN_FIRST_RECOVERY)){
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTMDP RECOVERY STATUS=%08x TIMEOUTS=%u KICK=%u DONE=%u SEQ=%llu LIVE_PENDING=%u LATCHED=%u LATE_ACK=%u PENDING_REJECT=%u LAST_ACK=%08x old-failed-transaction-not-completed\n",d.first_recovery_status,d.first_recovery_pending_count,d.first_recovery_kicks,d.first_recovery_completed,(unsigned long long)d.first_recovery_sequence,d.pending,d.latched,d.late_ack_count,d.pending_rejects,d.last_ack_status);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  recoveryLogged=true;
 }
 return true;
}
/* Five one-shot protocol observations. K3 observes a valid zero-contact
 * state after an enqueued Down; K4 observes at least one enqueued Up.
 * Sampling may miss a short state. Neither proves application delivery. */
bool IOS7LeoSDLogger::observeTouch(bool logged[5],uint8_t *payload,uint8_t sector[512])
{
 LeoTouchObservation d;bzero(&d,sizeof(d));d.version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;
 if(!ios7leo_touch_observe(&d))return true;
 bool present[5]={d.init_valid!=0,d.enqueued!=0,d.reader_progress!=0,
  d.real_reports!=0 && d.last_sequence!=0 && d.last_down==0 && d.down_events!=0,
  d.up_events!=0};
 for(unsigned kind=0;kind<5;kind++)if(present[kind]&&!logged[kind]){
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTTOUCH K=%u HEX KN=%x INIT=%x/%x READY=%x ST=%x ID=%x PUB=%x PID=%x OPEN=%x MAP=%x NTF=%x RAW=%x ENQ=%x DMU=%x/%x/%x QERR=%x READ=%x HT=%x/%x PEND=%x LAST=%x/%x/%x/%x IOERR=%x LOSS=%x\n",kind,d.known,d.init_valid,(uint32_t)d.init_result,d.init_ready,d.init_stage,d.id_word,d.published,d.client_pid,d.opened,d.map_calls,d.notification_calls,d.real_reports,d.enqueued,d.down_events,d.move_events,d.up_events,d.queue_failures,d.reader_progress,d.reader_head,d.writer_tail,d.pending,d.last_sequence,d.last_x,d.last_y,d.last_down,d.transport_errors,d.loss);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  logged[kind]=true;
 }
 return true;
}
void IOS7LeoSDLogger::run()
{
 uint8_t payload[LEO_LOG_PAYLOAD],sector[512];
 int n=snprintf((char *)payload,sizeof(payload),"BOOT_START logger_start_ns=%llu CID=%08x:%08x:%08x:%08x identity=source/package(notunique_boot)\n",(unsigned long long)startNs,(unsigned)info.cid[0],(unsigned)info.cid[1],(unsigned)info.cid[2],(unsigned)info.cid[3]);
 if(next==LEO_LOG_SECTORS){loggerState=Full;IOLog("Leo log FULL; nooverwrite, noSDwrite\n");return;}
 if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;IOLog("Leo log FAILED: boot-start format return=%d\n",n);return;}
 result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),2,0,payload,(uint32_t)n);
 if(!result)result=ios7leo_sd_log_write(cap,next,sector);if(result){loggerState=Failed;IOLog("Leo log FAILED: boot-start write=%d; sharedSDfault mayaffectroot\n",result);return;}next++;loggerState=Active;
 IOLog("Leo log active BOOTLOG8.BIN next=%u first-write-readback=1\n",(unsigned)next);
 /* Six finite milestones also cover the user's 40-minute observation.
  * They run in this existing worker; they do not add a timer or thread. */
 static const uint32_t observation_seconds[6]={0,180,600,1200,1800,2400};
 static const uint32_t post_seconds[5]={0,10,60,180,600};
 bool firstSeen=false;uint64_t firstSeenNs=0;uint32_t postStage=0;
 uint32_t observation_stage=0;bool quotaNoted=false,faultIssueNoted=false;
 bool mdpFirstLogged=false,mdpRecoveryLogged=false;
 bool touchLogged[5]={false,false,false,false,false};
 for(;;){
  if(next==LEO_LOG_SECTORS){loggerState=Full;IOLog("Leo log FULL; committed through2047, nooverwrite\n");return;}
  if(!observeMDP(mdpFirstLogged,mdpRecoveryLogged,payload,sector))return;
  if(!observeTouch(touchLogged,payload,sector))return;
  /* First-fault evidence is one bounded immutable capture. Drain it before
   * ordinary detail, including after the detail quota. No producer performs IO. */
  uint32_t faultLoss=0,faultUnknown=0;
  uint32_t faultBytes=ios7lab_fault_witness_take(payload,LEO_LOG_PAYLOAD,&faultLoss,&faultUnknown);
  if(faultBytes){
   result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,faultUnknown?UINT32_MAX:faultLoss,payload,faultBytes);
   if(!result)result=ios7leo_sd_log_write(cap,next,sector);
   if(result){loggerState=Failed;IOLog("Leo first-fault write failed=%d next=%u record-popped/no-retry\n",result,(unsigned)next);return;}
   next++;IOSleep(20);continue;
  }
  if((faultLoss||faultUnknown)&&!faultIssueNoted){
   int issue=snprintf((char *)payload,LEO_LOG_PAYLOAD,"IOS7LAB FIRSTFAULT ISSUE LOST=%u UNKNOWN=%u partial-or-missing-evidence-not-no-fault\n",faultLoss,faultUnknown);
   if(issue<=0||(uint32_t)issue>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return;}
   if(!appendObservation(payload,(uint32_t)issue,sector))return;faultIssueNoted=true;
  }
  LeoGraphicsEvent event;bzero(&event,sizeof(event));uint32_t eventLost=0;
  if(ios7leo_graphics_event_take(&event,&eventLost)){
   int eventBytes=snprintf((char *)payload,LEO_LOG_PAYLOAD,"GFXEVENT S=%u K=%u CLIENT=%x PID=%u SEL=%u RESULT=%x REASON=%x TX=%x:%x SURFACE=%u MISMATCH=%x GEN=%x:%x REQ=%u ENABLE=%u CLOSED=%u PORT=%u CALLBACK=%u SEND=%x:%x LOSS=%u\n",event.sequence,event.kind,event.client,event.owner_pid,event.selector,event.result,event.reason,event.transaction_hi,event.transaction_lo,event.surface,event.mismatch,event.generation_hi,event.generation_lo,event.requested,event.enabled,event.closed,event.port_present,event.callback_present,event.send_count_hi,event.send_count_lo,eventLost);
   if(eventBytes<=0||(uint32_t)eventBytes>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return;}
   if(!appendObservation(payload,(uint32_t)eventBytes,sector))return;
  }
  uint64_t observation_now=log_now_ns();
  LeoGraphicsState current;bzero(&current,sizeof(current));current.version=LEO_GRAPHICS_STATE_VERSION;
  if(!firstSeen&&ios7leo_graphics_snapshot(&current)&&current.selector5_completed){firstSeen=true;firstSeenNs=observation_now;}
  if(firstSeen&&postStage<5&&observation_now>=firstSeenNs&&observation_now-firstSeenNs>=(uint64_t)post_seconds[postStage]*1000000000ULL){
   int phaseBytes=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTPHASE M=%u FIRST_SEEN_NS=%llu NOW_NS=%llu SINCE_FIRST_NS=%llu REQUESTED=%u completed-frame-observer-not-first-pixel-time\n",100U+postStage,(unsigned long long)firstSeenNs,(unsigned long long)observation_now,(unsigned long long)(observation_now-firstSeenNs),post_seconds[postStage]);
   if(phaseBytes<=0||(uint32_t)phaseBytes>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return;}
   if(!appendObservation(payload,(uint32_t)phaseBytes,sector))return;
   uint64_t requested=(firstSeenNs>=startNs?(firstSeenNs-startNs)/1000000000ULL:0)+post_seconds[postStage];
   if(!observe(100U+postStage,requested,observation_now,payload,sector))return;
   postStage++;
  }
  if(observation_stage<6 && (observation_stage==0 || (observation_now>=startNs && observation_now-startNs>=(uint64_t)observation_seconds[observation_stage]*1000000000ULL))){
   if(!observe(observation_stage,observation_seconds[observation_stage],observation_now,payload,sector))return;
   observation_stage++;
  }
  if(next>=LEO_LOG_SECTORS-256U && !quotaNoted){
   int quota=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS QUOTA ordinary-console-stop=1792 trace-stop=1856 last192-reserved-fault-graphics-memory-MDP-and-post-present noreset/nooverwrite/screenunchanged NEXT=%u\n",(unsigned)next);
   if(quota<=0||(uint32_t)quota>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;return;}
   if(!appendObservation(payload,(uint32_t)quota,sector))return;quotaNoted=true;
  }
  uint32_t traceLoss=0,traceUnknown=0;
  uint32_t traceBytes=next<LEO_LOG_SECTORS-192U?ios7leo_frontier_take(payload,LEO_LOG_PAYLOAD,&traceLoss,&traceUnknown):0;
  if(traceBytes){
   result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,traceUnknown?UINT32_MAX:traceLoss,payload,traceBytes);
   if(!result)result=ios7leo_sd_log_write(cap,next,sector);
   if(result){loggerState=Failed;IOLog("Leo UIEVENT write failed=%d next=%u (record popped, no retry/sharedfault preserved)\n",result,(unsigned)next);return;}
   next++;
   if(next==LEO_LOG_SECTORS){loggerState=Full;IOLog("Leo log FULL after UIEVENT; no overwrite\n");return;}
  }
  /* Stop only ordinary SD console appends. Producers/screen remain original;
   * uncommitted spool bytes and actual drops remain honest. Reserved tail is
   * sequential, without holes, and the same worker still checks milestones. */
  if(next>=LEO_LOG_SECTORS-256U){IOSleep(traceBytes?20:200);continue;}
  uint32_t bytes=0,dropped=0;
  if(OSCompareAndSwap(0,1,&spoolGuard)){
   bytes=leo_log_spool_snapshot(&spool,payload,LEO_LOG_PAYLOAD);
   uint64_t total=(uint64_t)spool.dropped+busyDrops;dropped=unknownDrops||total>UINT32_MAX?UINT32_MAX:(uint32_t)total;
   OSMemoryBarrier();spoolGuard=0;
  }
  if(!bytes){IOSleep(traceBytes?20:200);continue;}
  result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,dropped,payload,bytes);
  if(!result)result=ios7leo_sd_log_write(cap,next,sector);
  if(result){loggerState=Failed;IOLog("Leo log FAILED: write=%d next=%u drops=%u unknown-drop=%u (no retry/sharedfault preserved)\n",result,(unsigned)next,(unsigned)dropped,(unsigned)unknownDrops);return;}
  /* No producer changes head/count except addingtail; onlythisworker commits. */
  while(!OSCompareAndSwap(0,1,&spoolGuard))IOSleep(1);
  result=leo_log_spool_commit(&spool,bytes);OSMemoryBarrier();spoolGuard=0;
  if(result){loggerState=Failed;IOLog("Leo log FAILED: internalcommit=%d; nofurtherwrites\n",result);return;}next++;IOSleep(20);
 }
}
void IOS7LeoSDLogger::releaseResources()
{
 if(opened&&raw){raw->close(this);}
 if(raw){raw->release();}
 if(device){device->release();}
 if(bounce){bounce->release();}
 if(work){IOFree(work,sizeof(*work));}
 if(ext){IOFree(ext,128U*sizeof(*ext));}
 if(dirs){IOFree(dirs,LEO_LOG_DIR_CAP*sizeof(*dirs));}
 if(owners){IOFree(owners,(LEO_LOG_CLUSTER_CAP+9U)/8U);}
 if(ranges){IOFree(ranges,128U*sizeof(*ranges));}
 opened=false;raw=0;device=0;bounce=0;work=0;ext=0;dirs=0;owners=0;ranges=0;
}
void IOS7LeoSDLogger::free()
{
 releaseResources();IOService::free();
}
extern "C" void ios7leo_prepare_log_reader(void)
{
 const char *xml="<array><dict><key>IOClass</key><string>IOS7LeoSDLogger</string><key>IOProviderClass</key><string>IOMedia</string><key>IOMatchCategory</key><string>IOS7LeoOwnedLog</string><key>IOPropertyMatch</key><dict><key>Whole</key><true/></dict><key>IOProbeScore</key><integer>10000</integer></dict></array>";
 OSObject *o=OSUnserializeXML(xml);OSArray *a=OSDynamicCast(OSArray,o);
 if(!a||!gIOCatalogue->addDrivers(a,false))IOLog("Leo log catalogue disabled (bootcontinues)\n");if(o)o->release();
}
#endif
