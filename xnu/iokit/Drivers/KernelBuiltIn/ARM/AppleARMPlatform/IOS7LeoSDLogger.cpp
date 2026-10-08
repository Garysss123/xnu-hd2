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
extern "C" void ios7leo_log_progress(uint32_t,uint32_t);
extern "C" uint32_t ios7leo_prefkill059_take(uint8_t *,uint32_t);
#include "IOS7LeoSDLog.h"
#include "IOS7LeoErrorLog.h"
#include "IOS7LeoWaitLog.h"
#include "IOS7LeoSurfaceAdmission.h"
#include "IOS7LeoSwitchObservation.h"
#include "IOS7LeoVmRangeObservation.h"
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
#include "leo_touch_release_watch.h"
#include "IOS7LeoPresentCost.h"
extern "C" void ios7leo_mdp_ack_observe_v2(uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *);
extern "C" {
#include "../../../../../osfmk/arm/ios7lab_fault_witness_shared.h"
#include "../../../../../osfmk/arm/ios7leo_abort037_shared.h"
#include "../../../../../osfmk/arm/IOS7LeoAppFault057Shared.h"
#include "../../../../../osfmk/arm/IOS7LeoPreferencesWait058Shared.h"
#include "../../../../../osfmk/ipc/IOS7LeoPreferencesRPC060Shared.h"
#include "../../../../../osfmk/kern/IOS7LeoThreadState061Shared.h"
#include "../../../../../osfmk/kern/IOS7LeoQueuedState063Shared.h"
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
 int writeRecord(uint32_t,const uint8_t *);
 bool appendObservation(uint8_t *,uint32_t,uint8_t[512]);
 bool observe(uint32_t,uint64_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeActivity(uint32_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeWaitProgress(uint32_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observePreferences(uint64_t,uint8_t *,uint8_t[512]);
 bool appendAdmission(uint32_t,const LeoSurfaceAdmissionObservation &,uint8_t *,uint8_t[512]);
 bool appendInput(uint32_t,const LeoTouchObservation &,uint8_t *,uint8_t[512]);
 bool observeExtended(uint32_t,uint64_t,uint8_t *,uint8_t[512]);
 bool observeMDP(bool &,bool &,uint8_t *,uint8_t[512]);
 bool appendTouch(unsigned,const LeoTouchObservation &,uint8_t *,uint8_t[512]);
 bool observeTouch(bool[5],LeoTouchReleaseWatch &,uint8_t *,uint8_t[512]);
 LeoWaitState waitDiagnostics; /* resident member; sizeof(*this) charged byprepare */
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
 if(kr!=KERN_SUCCESS){loggerState=Failed;ios7leo_log_progress(5,next);release();IOLog("Leo log FAILED: worker creation=%d\n",kr);return false;}
 thread_deallocate(thread);return true;
}
void IOS7LeoSDLogger::worker(void *p,wait_result_t)
{
 IOS7LeoSDLogger *self=(IOS7LeoSDLogger *)p;
 if(!self->prepare()){
  self->loggerState=Failed;ios7leo_log_progress(5,self->next);
  IOLog("Leo log FAILED: prepare result=%d (no write; rootpolicy unchanged)\n",self->result);
  self->releaseResources();
 }else{self->releaseResources();self->run();}
 self->release();thread_terminate_self();
}
int IOS7LeoSDLogger::writeRecord(uint32_t logical,const uint8_t *sector)
{
 ios7leo_log_progress(2,logical);
 int r=ios7leo_sd_log_write(cap,logical,sector);
 ios7leo_log_progress(r?5:3,logical);return r;
}
bool IOS7LeoSDLogger::appendObservation(uint8_t *payload,uint32_t bytes,uint8_t sector[512])
{
 if(next>=LEO_LOG_SECTORS){loggerState=Full;ios7leo_log_progress(6,next);return false;}
 result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,0,payload,bytes);
 if(!result)result=writeRecord(next,sector);
 if(result){loggerState=Failed;ios7leo_log_progress(5,next);IOLog("Leo observation write failed=%d next=%u no retry/sharedfault preserved\n",result,(unsigned)next);return false;}
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
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS M=%u PART=2 NOW_NS=%llu KNOWN=%x SEQ=%llu:%llu SCAN_HASH=%08x SCAN_RGB=%u SCAN_N=%u PRIMARY_HASH=%08x PRIMARY_RGB=%u PRIMARY_ALPHA=%u PRIMARY_N=%u BYTES=%u SEED=%u:%u SURFACE=%u 1024-grid-only;metadata-stability-not-atomic-user-frame\n",milestone,(unsigned long long)now,d.known,(unsigned long long)d.completed_sequence_before,(unsigned long long)d.completed_sequence_after,d.scanout_sample_hash,d.scanout_nonzero_rgb,d.scanout_samples,d.primary_sample_hash,d.primary_nonzero_rgb,d.primary_nonzero_alpha,d.primary_samples,d.primary_bytes,d.primary_seed_before,d.primary_seed_after,d.active_surface);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTOBS M=%u PART=3 NOW_NS=%llu SERVICE_KNOWN=%u PID=%u TID=%llu REQ=%u REP=%u LAST_REQ=%u LAST_REP=%u STATUS_KIND=%u STATUS=%x DISPOSITION=%u SAT=%u PAIRS=%u LIMIT=%u LATE_WAIT=%u LOSS=%u UNK=%u LAST_TICK_ABS=%llu same-target-only;tryguard-loss-qualified\n",milestone,(unsigned long long)now,activityKnown,a.pid,(unsigned long long)a.tid,a.requests,a.replies,a.last_request,a.last_reply,a.status_kind,a.status,a.disposition,a.saturated,a.service_pairs,a.limit_reached,a.late_wait,a.loss,a.unknown,(unsigned long long)a.last_tick);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
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
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 for(uint32_t start=0;start<IOS7LAB_FAULT_WITNESS_IRQ_JOBS+1U;start+=3U){
  n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTACT M=%u PART=%u ROLE:SEEN,PID,TID,COUNT,PC,SP,LR,CPSR ",milestone,1U+start/3U);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  uint32_t used=(uint32_t)n;
  for(uint32_t i=start;i<start+3U&&i<IOS7LAB_FAULT_WITNESS_IRQ_JOBS+1U;i++){
   const ios7lab_irq_job_snapshot *j=&sample.jobs[i];
   n=snprintf((char *)payload+used,LEO_LOG_PAYLOAD-used,"[%u:%u,%u,%llu,%u,%08x,%08x,%08x,%08x] ",i,(unsigned)(j->count!=0),j->pid,(unsigned long long)j->tid,j->count,j->last_pc,j->last_sp,j->last_lr,j->last_cpsr);
   if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD-used){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
   used+=(uint32_t)n;
  }
  if(used+1U>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
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
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=2 KNOWN=%u CID=%x PID=%u LAST=%u/%x/%x TX=%x:%x SURFACE=%u MM=%x WAIT=%x:%x CFG=%u/%u/%u/%u/%u GEN=%x:%x SENDSTAT=%x TIMERSTAT=%x EVENTS=%u:%u aggregate-last-client;not-physical-vsync\n",milestone,known,g.last_client,g.owner_pid,g.last_selector,g.last_result,g.last_reason,g.last_transaction_hi,g.last_transaction_lo,g.last_surface,g.last_mismatch,g.last_wait_hi,g.last_wait_lo,g.requested,g.enabled,g.closed,g.port_present,g.callback_present,g.generation_hi,g.generation_lo,g.last_send_status,g.last_timer_status,g.events_published,g.events_taken);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=3 KNOWN=%u RESULT4=%x REASON4=%x RESULT5=%x REASON5=%x RESULT6=%x REASON6=%x RESULT9=%x REASON9=%x failures-before-PE-are-included\n",milestone,known,g.selector4_result,g.selector4_reason,g.selector5_result,g.selector5_reason,g.selector6_result,g.selector6_reason,g.selector9_result,g.selector9_reason);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTGFX M=%u PART=4 OPEN=%x/%u Q3=%u/%u Q8=%u/%u Q18=%u/%u QLAST=%x/%u/%u/%x PIPE=%u/%u/%u/%u/%u PENTRY=%x/%u/%u/%u RET=%x historical-identities\n",milestone,g.latest_open_client,g.latest_open_pid,g.selector3_calls,g.selector3_successes,g.selector8_calls,g.selector8_successes,g.selector18_calls,g.selector18_successes,g.prereq_last_client,g.prereq_last_pid,g.prereq_last_selector,g.prereq_last_result,g.port_entries,g.port_preflight_rejects,g.port_gate_entries,g.port_calls,g.port_returns,g.port_entry_client,g.port_entry_pid,g.port_entry_type,g.port_entry_valid,g.port_last_return);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 LeoPresentCostObservation cost;bzero(&cost,sizeof(cost));cost.version=1;
 ios7leo_present_cost_observe(&cost);
 uint32_t ack_ops=0,ack_delayed=0,ack_max=0,ack_fail=0,ack_clears=0,ack_diag=0;
 ios7leo_mdp_ack_observe_v2(&ack_ops,&ack_delayed,&ack_max,&ack_fail,&ack_clears,&ack_diag);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTCOST M=%u KNOWN=%x ATT=%u DONE=%u SEQ=%llu CONV=%llu POLL=%llu CTOT=%llu PTOT=%llu ROW=%u RSAMPLES=%u RSQ=%llu R0=%llu RMAX=%llu ACK2=%x/%x/%x/%x/%x/%x wall-ticks\n",milestone,cost.known,cost.attempts,cost.completed,(unsigned long long)cost.last_completed_sequence,(unsigned long long)cost.last_conversion_ticks,(unsigned long long)cost.last_poll_ticks,(unsigned long long)cost.total_conversion_ticks,(unsigned long long)cost.total_poll_ticks,cost.last_conversion_rows,cost.row_samples,(unsigned long long)cost.last_row_sample_sequence,(unsigned long long)cost.last_row_sample_ticks,(unsigned long long)cost.max_row_sample_ticks,ack_ops,ack_delayed,ack_max,ack_fail,ack_clears,ack_diag);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 LeoMemoryObservation v;bzero(&v,sizeof(v));v.version=IOS7_LEO_MEMORY_OBS_VERSION;
 ios7leo_memory_observe(&v);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTMEM M=%u NS=%llu KNOWN=%x STATUS=%u SEQ=%u SELECTED=%08x:%08x MANAGED=%llu FREE=%u RESERVE=%u TARGET=%u PAGEINS=%llu PAGEOUTS=%llu FAULTS=%llu BANKS=%u ADMIT=%u sampled-master-counters-not-process-cost\n",milestone,(unsigned long long)now,v.known,v.status,v.sequence,v.selected_base,v.selected_bytes,(unsigned long long)v.managed_bytes,v.free_pages,v.free_reserved,v.free_target,(unsigned long long)v.pageins,(unsigned long long)v.pageouts,(unsigned long long)v.faults,v.bank_count,(unsigned)((v.known&LEO_MEMORY_KNOWN_BOARD_PROFILE)!=0));
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 if(milestone==0)for(uint32_t start=0;start<IOS7_LEO_MEMORY_MAX_BANKS;start+=4U){
  n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTBANK PART=%u KNOWN=%x STATUS=%u COUNT=%u ATAG=%08x:%08x SELECTED=%08x:%08x ",1U+start/4U,v.known,v.status,v.bank_count,v.atag_base,v.atag_end,v.selected_base,v.selected_bytes);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  uint32_t used=(uint32_t)n;
  for(uint32_t i=start;i<start+4U;i++){
   n=snprintf((char *)payload+used,LEO_LOG_PAYLOAD-used,"B%u=%08x:%08x ",i,v.banks[i].base,v.banks[i].bytes);
   if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD-used){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}used+=(uint32_t)n;
  }
  if(used+1U>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
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
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  firstLogged=true;
  uint32_t ops=0,delayed=0,rounds=0,failures=0,clears=0,diagnostic=0;
  ios7leo_mdp_ack_observe_v2(&ops,&delayed,&rounds,&failures,&clears,&diagnostic);
  n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"MDP_ERROR_ACK2 %x/%x/%x/%x/%x/%x after-first-error-observation;diag-issued-not-completed\n",ops,delayed,rounds,failures,clears,diagnostic);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
 }
 if(!recoveryLogged && d.first_recovery_valid && (d.known&LEO_MDP_KNOWN_FIRST_RECOVERY)){
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTMDP RECOVERY STATUS=%08x TIMEOUTS=%u KICK=%u DONE=%u SEQ=%llu LIVE_PENDING=%u LATCHED=%u LATE_ACK=%u PENDING_REJECT=%u LAST_ACK=%08x old-failed-transaction-not-completed\n",d.first_recovery_status,d.first_recovery_pending_count,d.first_recovery_kicks,d.first_recovery_completed,(unsigned long long)d.first_recovery_sequence,d.pending,d.latched,d.late_ack_count,d.pending_rejects,d.last_ack_status);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  recoveryLogged=true;
 }
 return true;
}
/* Five one-shot protocol observations. K3 observes a valid zero-contact
 * state after an enqueued Down; K4 observes at least one enqueued Up.
 * Sampling may miss a short state. Neither proves application delivery. */
bool IOS7LeoSDLogger::appendTouch(unsigned kind,const LeoTouchObservation &d,
    uint8_t *payload,uint8_t sector[512])
{
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTTOUCH K=%u HEX KN=%x INIT=%x/%x READY=%x ST=%x ID=%x PUB=%x PID=%x OPEN=%x MAP=%x NTF=%x RAW=%x ENQ=%x DMU=%x/%x/%x QERR=%x READ=%x HT=%x/%x PEND=%x LAST=%x/%x/%x/%x IOERR=%x LOSS=%x\n",kind,d.known,d.init_valid,(uint32_t)d.init_result,d.init_ready,d.init_stage,d.id_word,d.published,d.client_pid,d.opened,d.map_calls,d.notification_calls,d.real_reports,d.enqueued,d.down_events,d.move_events,d.up_events,d.queue_failures,d.reader_progress,d.reader_head,d.writer_tail,d.pending,d.last_sequence,d.last_x,d.last_y,d.last_down,d.transport_errors,d.loss);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTTOUCH K=%u PART=2 V=2 HEX EPOCH=%x UP=%x/%x/%x/%x XY=%x/%x UPN=%x UPTS=%llx SID=%llx NOTIFY=%x/%x/%x/%x/%x KR=%x NT=%x FLAGS=%x\n",kind,d.queue_epoch,d.last_up_epoch,d.last_up_enqueued,d.last_up_tail,d.last_up_sequence,d.last_up_x,d.last_up_y,d.last_up_notify_flags,(unsigned long long)d.last_up_timestamp,(unsigned long long)d.service_sender,d.notify_calls,d.notify_ok,d.notify_busy,d.notify_errors,d.notify_no_port,d.last_notify_result,d.last_notify_tail,d.observation_flags);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 return appendObservation(payload,(uint32_t)n,sector);
}
bool IOS7LeoSDLogger::observeTouch(bool logged[5],LeoTouchReleaseWatch &watch,
    uint8_t *payload,uint8_t sector[512])
{
 LeoTouchObservation d;bzero(&d,sizeof(d));d.version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;
 if(!ios7leo_touch_observe(&d))return true;
 uint64_t observed_now=log_now_ns();
 bool present[5]={d.init_valid!=0,d.enqueued!=0,d.reader_progress!=0,
  d.real_reports!=0 && d.last_sequence!=0 && d.last_down==0 && d.down_events!=0,
  d.up_events!=0};
 for(unsigned kind=0;kind<5;kind++)if(present[kind]&&!logged[kind]){
  if(!appendTouch(kind,d,payload,sector))return false;logged[kind]=true;
 }
 LeoTouchReleaseSample sample;bzero(&sample,sizeof(sample));
 if(leo_touch_release_sample(&watch,&d,observed_now,&sample)){
  int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"BOOTRELEASE SAMPLE=%u DELAY=%u OBSERVED_UP_NS=%llu NOW_NS=%llu ELAPSED_NS=%llu FIFO_REMOVED=%u CAP=4 observer-anchor-not-hardware-up;removed-not-gesture-acceptance\n",sample.ordinal,sample.delay_seconds,(unsigned long long)sample.observed_up_ns,(unsigned long long)sample.now_ns,(unsigned long long)sample.elapsed_ns,sample.queue_removed);
  if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){result=kIOReturnNoSpace;loggerState=Failed;ios7leo_log_progress(5,next);return false;}
  if(!appendObservation(payload,(uint32_t)n,sector))return false;
  if(!appendTouch(100U+sample.ordinal,d,payload,sector))return false;
 }
 return true;
}

void IOS7LeoSDLogger::run()
{
 uint8_t payload[LEO_LOG_PAYLOAD],input[LEO_LOG_PAYLOAD],sector[512];
 int n=snprintf((char *)payload,sizeof(payload),"BOOT_START logger_start_ns=%llu CID=%08x:%08x:%08x:%08x identity=source/package(notunique_boot)\n",(unsigned long long)startNs,(unsigned)info.cid[0],(unsigned)info.cid[1],(unsigned)info.cid[2],(unsigned)info.cid[3]);
 if(next==LEO_LOG_SECTORS){loggerState=Full;ios7leo_log_progress(6,next);return;}
 if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;ios7leo_log_progress(5,next);return;}
 result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),2,0,payload,(uint32_t)n);
 if(!result)result=writeRecord(next,sector);
 if(result){loggerState=Failed;ios7leo_log_progress(5,next);return;}
 next++;loggerState=Active;
 IOLog("Leo log active BOOTLG33.BIN next=%u first-write-readback=1 bounded-diagnostics\n",(unsigned)next);
 const char policy[]="LOG_POLICY ERRORS_BOUNDED_DIAG console=0 normal_gfx=0 ERR64 CRIT16 GFX32 WAIT32+1_LIMIT REPEAT_MSGS3/10/100/1000 PAIRS30/120s IRQ_STATE@480/900/1200_worker_seconds SURFACE3+2 INPUT3 SWITCH3 VMRANGE3 APPFAULT89_ALLPID ASL_BODY COPYCOST3 PREFWAIT10x6_QUEUED_STATE ATSTEP4 UI64 PREFKILL8_RETURN AX_OBJECT_DEDUP noHEALTH noUIproof gaps_possible\n";
 if(!appendObservation((uint8_t *)policy,sizeof(policy)-1,sector))return;
 LeoErrorLine line;leo_error_reset(&line);leo_wait_reset(&waitDiagnostics);
 LeoErrorBudget errorBudget;leo_error_budget_reset(&errorBudget);
 uint32_t diagStage=0;const uint32_t diagSeconds[3]={480,900,1200};
 bool denialLogged=false,belowOldLogged=false,inputDownLogged=false,inputUpLogged=false,inputTailLogged=false;uint64_t firstUpObserved=0;
 uint32_t gfxLeft=32,gfxKeys[32]={0},gfxCount=0;
 uint32_t atSteps058=0,uiSteps059=0;
 bool mdpFirstLogged=false,mdpRecoveryLogged=false,faultIssueNoted=false,consoleLossNoted=false;
 uint32_t lastConsoleLoss=0,lastConsoleUnknown=0;
 for(;;){
  uint64_t observedNow=log_now_ns();
  ios7leo_log_progress(1,next);
  if(next==LEO_LOG_SECTORS){loggerState=Full;ios7leo_log_progress(6,next);return;}
  if(!observeMDP(mdpFirstLogged,mdpRecoveryLogged,payload,sector))return;
  uint32_t appFaultBytes=ios7leo_appfault057_take(payload,LEO_LOG_PAYLOAD);
  if(appFaultBytes){
   if(appFaultBytes>LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return;}
   if(!appendObservation(payload,appFaultBytes,sector))return;
  }
  if(!observePreferences(observedNow,payload,sector))return;
  uint32_t killBytes=ios7leo_prefkill059_take(payload,LEO_LOG_PAYLOAD);
  if(killBytes){if(!appendObservation(payload,killBytes,sector))return;}
  uint32_t faultBytes=ios7leo_abort037_take(payload,LEO_LOG_PAYLOAD);
  if(faultBytes){
   if(faultBytes>LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return;}
   if(!appendObservation(payload,faultBytes,sector))return;
  }
  uint32_t faultLoss=0,faultUnknown=0;
  faultBytes=ios7lab_fault_witness_take(payload,LEO_LOG_PAYLOAD,&faultLoss,&faultUnknown);
  if(faultBytes){
   result=leo_log_record_make(sector,ios7leo_log_nonce,ios7leo_log_package_identity,next,log_now_ns(),1,faultUnknown?UINT32_MAX:faultLoss,payload,faultBytes);
   if(!result)result=writeRecord(next,sector);
   if(result){loggerState=Failed;ios7leo_log_progress(5,next);return;}++next;
  }
  if((faultLoss||faultUnknown)&&!faultIssueNoted){
   n=snprintf((char *)payload,sizeof(payload),"FIRSTFAULT_ERROR lost=%u unknown=%u partial_evidence\n",faultLoss,faultUnknown);
   if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;ios7leo_log_progress(5,next);return;}
   if(!appendObservation(payload,n,sector))return;faultIssueNoted=true;
  }
  /* Successful graphics events are drained in RAM, never written to SD. */
  for(unsigned take=0;take<16;take++){
   LeoGraphicsEvent e;bzero(&e,sizeof(e));uint32_t lost=0;
   if(!ios7leo_graphics_event_take(&e,&lost))break;
   if(!e.result||!gfxLeft)continue;
   uint32_t key=2166136261U;key=(key^e.owner_pid)*16777619U;key=(key^e.selector)*16777619U;key=(key^e.result)*16777619U;key=(key^e.reason)*16777619U;key=(key^e.mismatch)*16777619U;
   bool seen=false;for(uint32_t i=0;i<gfxCount;i++)if(gfxKeys[i]==key)seen=true;if(seen)continue;
   n=snprintf((char *)payload,sizeof(payload),"GFXERROR S=%u PID=%u SEL=%u RESULT=%x REASON=%x TX=%x:%x SURFACE=%u MISMATCH=%x LOSS=%u\n",e.sequence,e.owner_pid,e.selector,e.result,e.reason,e.transaction_hi,e.transaction_lo,e.surface,e.mismatch,lost);
   if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;ios7leo_log_progress(5,next);return;}
   if(!appendObservation(payload,n,sector))return;
   gfxKeys[gfxCount++]=key;--gfxLeft;
  }
  /* Two allocation milestones and one real gesture sequence, from the same
   * worker. Try/RAM-only accessors finish before SD; never a retry policy. */
  if(!denialLogged||!belowOldLogged){
   LeoSurfaceAdmissionObservation a;bzero(&a,sizeof(a));a.version=1;ios7leo_surface_admission_observe(&a);
   if(a.known==1){
    if(a.denials&&!denialLogged){if(!appendAdmission(1,a,payload,sector))return;denialLogged=true;}
    if(a.below_old_admitted&&!belowOldLogged){if(!appendAdmission(2,a,payload,sector))return;belowOldLogged=true;}
   }
  }
  if(!inputTailLogged){
   LeoTouchObservation t;bzero(&t,sizeof(t));t.version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;ios7leo_touch_observe(&t);
   if(t.down_events&&!inputDownLogged){if(!appendInput(1,t,payload,sector))return;inputDownLogged=true;}
   if(t.up_events&&!inputUpLogged){if(!appendInput(2,t,payload,sector))return;inputUpLogged=true;firstUpObserved=observedNow;}
   if(inputUpLogged&&observedNow>=firstUpObserved&&observedNow-firstUpObserved>=5000000000ULL){
    if(!appendInput(3,t,payload,sector))return;inputTailLogged=true;
   }
  }
  /* Bounded RAM batches continue even after error budgets are exhausted. */
  for(unsigned batch=0;batch<8;batch++){
   uint32_t bytes=0,loss=lastConsoleLoss,unknown=lastConsoleUnknown;
   if(OSCompareAndSwap(0,1,&spoolGuard)){
    bytes=leo_log_spool_snapshot(&spool,input,sizeof(input));
    result=bytes?leo_log_spool_commit(&spool,bytes):0;
    uint64_t total=(uint64_t)spool.dropped+busyDrops;unknown=unknownDrops||total>UINT32_MAX;
    loss=total>UINT32_MAX?UINT32_MAX:(uint32_t)total;OSMemoryBarrier();spoolGuard=0;
    if(result){loggerState=Failed;ios7leo_log_progress(5,next);return;}
   }
   if(loss!=lastConsoleLoss||unknown!=lastConsoleUnknown){
    leo_error_reset(&line);line.truncated=1;line.capture_gap=1;leo_wait_gap(&waitDiagnostics);lastConsoleLoss=loss;lastConsoleUnknown=unknown;
    if(!consoleLossNoted){
     n=snprintf((char *)payload,sizeof(payload),"LOG_CAPTURE_ERROR lost_lower_bound=%u unknown=%u line_context_reset incomplete_errors_possible\n",loss,unknown);
     if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;ios7leo_log_progress(5,next);return;}
     if(!appendObservation(payload,n,sector))return;consoleLossNoted=true;
    }
   }
   if(!bytes)break;
   for(uint32_t at=0;at<bytes;at++){
    int kind=leo_error_feed(&line,input[at]);if(!kind)continue;
    if(kind==3 && atSteps058<4U && !line.truncated && !line.capture_gap && !strncmp(line.text,"ATSTEP058 ",10U)){
     n=snprintf((char *)payload,sizeof(payload),"%s\n",line.text);
     if(n>0&&(unsigned)n<sizeof(payload)){if(!appendObservation(payload,n,sector))return;++atSteps058;}
     leo_error_reset(&line);continue;
    }
    if(kind==3 && uiSteps059<64U && !line.truncated && !line.capture_gap && !strncmp(line.text,"UIOBS061 ",9U)){
     n=snprintf((char *)payload,sizeof(payload),"%s\n",line.text);
     if(n>0&&(unsigned)n<sizeof(payload)){if(!appendObservation(payload,n,sector))return;++uiSteps059;}
     leo_error_reset(&line);continue;
    }
    int admit=kind<=2?leo_error_admit(&errorBudget,&line,kind):0;
    uint32_t occurrences=kind<=2?leo_error_occurrences(&errorBudget,&line,kind):0;
    leo_wait_note(&waitDiagnostics,&line,kind,occurrences,observedNow);
    if(!admit){leo_error_reset(&line);continue;}
    n=snprintf((char *)payload,sizeof(payload),"ERROR C=%u PID=%u TRUNC=%u %s\n",(unsigned)(kind==2),line.pid,line.truncated,line.text);
    if(n<=0||(unsigned)n>=sizeof(payload)){loggerState=Failed;ios7leo_log_progress(5,next);return;}
    if(!appendObservation(payload,n,sector))return;
    leo_error_reset(&line);
   }
  }
  LeoWaitReport report;
  if(leo_wait_poll(&waitDiagnostics,log_now_ns(),&report)){
   n=snprintf((char *)payload,sizeof(payload),LEO_WAIT_RECORD_FORMAT,report.reason,report.pid,report.key,report.count,report.flags,report.reported_valid,report.reported_retry,report.paired,report.wait_id,(unsigned long long)report.tracked_span_ns,(unsigned long long)report.observed_wait_ns,report.gaps,report.untracked_messages,report.clock_backwards,(unsigned)(report.source_trunc||leo_error_length(report.text)>80),report.text);
   if(n>0&&(unsigned)n<sizeof(payload)){if(!appendObservation(payload,n,sector))return;}
  }
  /* Only three finite snapshots from the existing worker; not periodic HEALTH.
   * Accessors copy RAM and restore IRQ/try guards before formatting or SD. */
  uint64_t sampled=log_now_ns();
  if(diagStage<3 && sampled>=startNs && sampled-startNs>=(uint64_t)diagSeconds[diagStage]*1000000000ULL){
   uint32_t requested=diagSeconds[diagStage++];
   if(!observeWaitProgress(requested,sampled,payload,sector))return;
  }
  IOSleep(200);
 }
}
bool IOS7LeoSDLogger::appendAdmission(uint32_t tag,const LeoSurfaceAdmissionObservation &a,uint8_t *payload,uint8_t sector[512])
{
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"SURFACE_MEMORY TAG=%u KN=%x DENIED=%u FIRST_PHASE=%u FIRST_FREE_GUARD_CHARGE=%u/%u/%u FIRST_WH=%u/%u BELOW_OLD_ADMITTED=%u NOW_FREE_GUARD_OLD=%u/%u/%u BUDGET_HEAP_PRIMARY=%u/%u/%u fresh-free;published-heap;pages-guards/bytes-charge;not-UI-success\n",tag,a.known,a.denials,a.first_phase,a.first_free,a.first_guard,a.first_charge,a.first_width,a.first_height,a.below_old_admitted,a.current_free,a.current_guard,a.old_guard,a.budget,a.heap,a.primary);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 return appendObservation(payload,(uint32_t)n,sector);
}
bool IOS7LeoSDLogger::appendInput(uint32_t tag,const LeoTouchObservation &t,uint8_t *payload,uint8_t sector[512])
{
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"FIRST_INPUT TAG=%u KN=%x PID=%u OPEN=%u RAW_DOWN_MOVE_UP=%u/%u/%u Q_ENQ_READ_PENDING=%u/%u/%u XY_DOWN=%u/%u/%u EPOCH_UP=%u/%u NOTIFY_OK_BUSY_ERR=%u/%u/%u LOSS=%u rawphysical;normalizedwire-unchanged;dequeue-not-unlock\n",tag,t.known,t.client_pid,t.opened,t.down_events,t.move_events,t.up_events,t.enqueued,t.reader_progress,t.pending,t.last_x,t.last_y,t.last_down,t.queue_epoch,t.last_up_epoch,t.notify_ok,t.notify_busy,t.notify_errors,t.loss);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 return appendObservation(payload,(uint32_t)n,sector);
}
bool IOS7LeoSDLogger::observeWaitProgress(uint32_t requested,uint64_t now,uint8_t *payload,uint8_t sector[512])
{
 if(!observeActivity(requested,now,payload,sector))return false;
 LeoVmRangeObservation vr;bzero(&vr,sizeof(vr));vr.version=1;ios7leo_vm_range_observe(&vr);
 int vrlen=snprintf((char *)payload,LEO_LOG_PAYLOAD,"VM_RANGE REQ=%u KN=%x DEALLOC_CALL_OK=%u/%u PROTECT_CALL_OK=%u/%u REJECT_HIGH_TASK_BACKENDERR=%u/%u/%u NONZERO_DEALLOC_OK=%u LAST_ADDR_SIZE_PROT_RESULT=%x/%x/%x/%x real-backend-results;not-resident-RAM-or-UI-proof\n",requested,vr.known,vr.deallocate_calls,vr.deallocate_ok,vr.protect_calls,vr.protect_ok,vr.rejected_high,vr.wrong_task,vr.backend_errors,vr.nonzero_deallocate_ok,vr.last_address,vr.last_size,vr.last_protection,vr.last_result);
 if(vrlen<=0||(uint32_t)vrlen>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)vrlen,sector))return false;
 LeoSwitchObservation sw;bzero(&sw,sizeof(sw));sw.version=1;ios7leo_switch_observe(&sw);
 int swlen=snprintf((char *)payload,LEO_LOG_PAYLOAD,"SWITCH_ABI REQ=%u KN=%x OPT3_4_5=%u/%u/%u PAUSE_RESTORE=%u/%u WAIT_DEPRESS_HANDOFF=%u/%u/%u LAST_TID=%x:%x aggregate-real-paths;not-lock-acquisition-or-UI-proof\n",requested,sw.known,sw.calls3,sw.calls4,sw.calls5,sw.paused,sw.restored,sw.waits,sw.depresses,sw.handoffs,sw.last_tid_high,sw.last_tid_low);
 if(swlen<=0||(uint32_t)swlen>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)swlen,sector))return false;
 LeoSurfaceAdmissionObservation a;bzero(&a,sizeof(a));a.version=1;ios7leo_surface_admission_observe(&a);
 if(!appendAdmission(requested,a,payload,sector))return false;
 LeoMDPFaultObservation m;bzero(&m,sizeof(m));m.version=IOS7_LEO_MDP_OBS_VERSION;ios7leo_mdp_observe(&m);
 LeoTouchObservation t;bzero(&t,sizeof(t));t.version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;uint32_t known=ios7leo_touch_observe(&t);
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"WAITSTATE REQ_SEC=%u OBS_NS=%llu MDP_KN_PENDING_LATCH_LATEACK_REJECT=%x/%x/%x/%x/%x TOUCH_KN_UP_ENQ_READ_PENDING_LOSS=%x/%x/%x/%x/%x/%x late-worker-sample;IRQ-any-thread;dequeue-not-unlock;not-CPU-percent\n",requested,(unsigned long long)now,m.known,m.pending,m.latched,m.late_ack_count,m.pending_rejects,known,t.up_events,t.enqueued,t.reader_progress,t.pending,t.loss);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD)return true;
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 LeoPresentCostObservation cost;bzero(&cost,sizeof(cost));cost.version=1;ios7leo_present_cost_observe(&cost);
 LeoMemoryObservation v;bzero(&v,sizeof(v));v.version=IOS7_LEO_MEMORY_OBS_VERSION;ios7leo_memory_observe(&v);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"WAITPERF REQ_SEC=%u COST_KN_ATT_DONE=%x/%x/%x CONV_TICKS=%llu POLL_TICKS=%llu VM_KN=%x MANAGED=%llu FREE_RES_TGT=%x/%x/%x PAGEIN_OUT_FAULT=%llu/%llu/%llu wall-ticks-not-CPU;VM-not-SD-timing\n",requested,cost.known,cost.attempts,cost.completed,(unsigned long long)cost.total_conversion_ticks,(unsigned long long)cost.total_poll_ticks,v.known,(unsigned long long)v.managed_bytes,v.free_pages,v.free_reserved,v.free_target,(unsigned long long)v.pageins,(unsigned long long)v.pageouts,(unsigned long long)v.faults);
 if(n<=0||(uint32_t)n>=LEO_LOG_PAYLOAD)return true;
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,"COPYCOST REQ=%u KN=%x LAST_CONV_POLL=%llu/%llu ROWS=%u ROW_SAMPLES=%u ROW_SEQ=%llu ROW_LAST_MAX=%llu/%llu last-completed-frame;IRQ-off-first-row-sample;whole-frame-includes-preemption\n",requested,cost.known,(unsigned long long)cost.last_conversion_ticks,(unsigned long long)cost.last_poll_ticks,cost.last_conversion_rows,cost.row_samples,(unsigned long long)cost.last_row_sample_sequence,(unsigned long long)cost.last_row_sample_ticks,(unsigned long long)cost.max_row_sample_ticks);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,(uint32_t)n,sector))return false;
 return true;
}
bool IOS7LeoSDLogger::observePreferences(uint64_t now,uint8_t *payload,uint8_t sector[512])
{
 LeoPrefs058Snapshot p;
 if(!ios7leo_prefs058_take(now,&p))return true;
 int n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_PREFS058_META_FORMAT,
 p.ordinal,p.requested,(unsigned long long)p.first_ns,(unsigned long long)p.now_ns,p.pid,
 (unsigned long long)p.tid,p.status,p.active,p.state,(unsigned long long)p.wait_event,p.wait_result,
 p.continuation,p.uss_valid,p.threads_examined,p.irq_count,(unsigned long long)p.irq_abs,
 p.irq_pc,p.irq_sp,p.irq_lr,p.irq_cpsr,p.irq_r7);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,n,sector))return false;
 if(p.uss_valid){
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_PREFS058_REG_FORMAT,
 p.ordinal,p.pid,(unsigned long long)p.tid,p.uss_valid,p.pc,p.sp,p.lr,p.cpsr,
 p.r[0],p.r[1],p.r[2],p.r[3],p.r[4],p.r[5],p.r[6],p.r[7],p.r[8],p.r[9],p.r[10],p.r[11],p.r[12]);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,n,sector))return false;
 }
 LeoPrefsRPC060 rpc;bzero(&rpc,sizeof(rpc));ios7leo_prefsrpc060_observe(&rpc);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_RPC060_FORMAT,
 p.ordinal,rpc.known,rpc.pid,rpc.pidversion,(unsigned long long)rpc.tid,rpc.call,rpc.stage,rpc.option,
 rpc.send_size,rpc.receive_limit,rpc.receive_name,rpc.timeout,rpc.header_valid,rpc.message_id,rpc.header_size,rpc.bits,rpc.remote_name,rpc.local_name,
 rpc.receive_header_valid,rpc.received_id,rpc.received_size,rpc.receive_state,rpc.result,
 (unsigned long long)rpc.entry_absolute,(unsigned long long)rpc.last_absolute,(unsigned long long)rpc.message_address,(unsigned long long)rpc.receive_address,rpc.active_receive);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,n,sector))return false;
 LeoThreadState061 t;bzero(&t,sizeof(t));ios7leo_threadstate061_take(&t);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_TS061_META_FORMAT,p.ordinal,t.known,t.active,t.call,t.pid,t.version,(unsigned long long)t.caller_tid,(unsigned long long)t.target_tid,t.target_ptr,t.same_task,t.flavor,t.request_count,t.phase,t.result,t.held_known,t.suspend_count,t.user_stop_count,(unsigned long long)t.begin_abs,(unsigned long long)t.phase_abs);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,n,sector))return false;
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_TS061_STATE_FORMAT,p.ordinal,t.event,t.event_count,t.state,t.sched_flags,t.ast,t.runq,t.processor,t.on_processor,t.wake_active,(unsigned long long)t.wait_event,t.continuation,t.wait_susp,t.wait_run,t.block_returns,t.wake_attempts,t.clear_runs,t.last_wresult,t.irq_user,t.irq_kernel,t.irq_pc,t.irq_lr,t.irq_psr,(unsigned long long)t.event_abs,(unsigned long long)t.irq_abs);
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 if(!appendObservation(payload,n,sector))return false;
 LeoQueuedState063 q;bzero(&q,sizeof(q));ios7leo_queuedstate063_observe(&q);
 n=snprintf((char *)payload,LEO_LOG_PAYLOAD,LEO_QUEUED063_FORMAT,p.ordinal,q.attempts,q.eligible,q.removed,q.requeued,q.success,q.backend_error,q.remove_failed,(unsigned long long)q.caller_tid,(unsigned long long)q.target_tid,q.last_state,q.last_priority,q.last_base_priority,q.last_count,q.last_result);
 if(p.ordinal==10U){ios7leo_prefsrpc060_stop();ios7leo_threadstate061_stop();}
 if(n<=0||(unsigned)n>=LEO_LOG_PAYLOAD){loggerState=Failed;ios7leo_log_progress(5,next);return false;}
 return appendObservation(payload,n,sector);
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
