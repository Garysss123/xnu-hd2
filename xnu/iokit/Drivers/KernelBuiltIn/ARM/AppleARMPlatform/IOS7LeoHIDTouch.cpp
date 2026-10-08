/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
#if defined(BOARD_CONFIG_QSD8250_LEO)
/* Real Type2A controller reports through the original iOS7 IOHIDLib queue.
 * PL050, synthetic contacts, buttons and userland routing helpers are absent. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#ifdef enqueue
#undef enqueue
#endif
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IODataQueueShared.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "IOS7LeoTouchTransport.h"
#include "IOS7LeoTouchWire.h"
#include "IOS7LeoTouchObservation.h"
#include "IOS7LeoTouchQueueProtocol.h"
extern "C" {
#include <mach/mach_time.h>
extern int proc_selfpid(void);
}
static volatile UInt32 leo_touch_observer_guard,leo_touch_observer_loss,leo_touch_observer_unknown;
static LeoTouchObservation leo_touch_observer_state;
static void leo_touch_observer_lost(void)
{
 for(unsigned n=0;n<4;n++){
  UInt32 old=leo_touch_observer_loss;if(old==UINT32_MAX)return;
  if(OSCompareAndSwap(old,old+1U,&leo_touch_observer_loss))return;
 }
 (void)OSCompareAndSwap(0,1,&leo_touch_observer_unknown);
}
extern "C" uint32_t ios7leo_touch_observe(LeoTouchObservation *out)
{
 if(!out || out->version!=IOS7_LEO_TOUCH_OBSERVATION_VERSION)return 0;
 if(!OSCompareAndSwap(0,1,&leo_touch_observer_guard)){leo_touch_observer_lost();return 0;}
 *out=leo_touch_observer_state;out->version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;
 out->loss=leo_touch_observer_unknown?UINT32_MAX:leo_touch_observer_loss;
 OSMemoryBarrier();leo_touch_observer_guard=0;return out->known?1U:0U;
}
static void leo_touch_inc(unsigned *value) {if(*value!=UINT32_MAX)++*value;}
/* HD2 is ARMv7. Keep the compiler clobber explicit rather than depending on
 * the legacy OSAtomic.h _ARM_ARCH_7 selection and its older CP15 variant. */
static void leo_touch_queue_barrier(void *) {__asm__ volatile("dmb sy" ::: "memory");}
static void leo_touch_queue_copy(void *,void *destination,const void *source,uint32_t bytes)
{memcpy(destination,source,bytes);}

class IOS7LeoHIDQueue : public IOSharedDataQueue {
 OSDeclareDefaultStructors(IOS7LeoHIDQueue)
 unsigned notifyCalls,notifyOK,notifyBusy,notifyErrors,notifyNoPort,lastNotifyResult,lastNotifyTail,lastEnqueueFlags;
 bool lastNotifyValid;
public:
 virtual Boolean initWithCapacity(UInt32 size) {
  notifyCalls=notifyOK=notifyBusy=notifyErrors=notifyNoPort=lastNotifyTail=lastEnqueueFlags=0;
  lastNotifyResult=UINT32_MAX;lastNotifyValid=false;return IOSharedDataQueue::initWithCapacity(size);
 }
 UInt32 readerHead(void) const {return dataQueue?dataQueue->head:0;}
 UInt32 writerTail(void) const {return dataQueue?dataQueue->tail:0;}
 UInt32 enqueueNotificationFlags(void) const {return lastEnqueueFlags;}
 bool portPresent(void) const {return notifyMsg && ((mach_msg_header_t *)notifyMsg)->msgh_remote_port!=MACH_PORT_NULL;}
 bool sane(void) const {
  return dataQueue && dataQueue->queueSize==4096 &&
   dataQueue->head<=4096 && dataQueue->tail<=4096 &&
   !(dataQueue->head&3U) && !(dataQueue->tail&3U);
 }
 /* Only while the exclusive consumer is opening/closing under service lock. */
 void discardAll(void) {if(dataQueue){dataQueue->head=dataQueue->tail=0;OSMemoryBarrier();}}
 virtual Boolean enqueue(void *data,UInt32 bytes) {
  lastEnqueueFlags=0;if(!dataQueue)return false;
  LeoTouchQueueHooks hooks={0,leo_touch_queue_barrier,leo_touch_queue_copy};
  uint32_t flags=0;
  if(!leo_touch_queue_enqueue(&dataQueue->head,&dataQueue->tail,dataQueue->queueSize,
   (unsigned char *)dataQueue->queue,data,bytes,&hooks,&flags))return false;
  lastEnqueueFlags=flags;
  if(flags&LEO_TOUCH_UP_NOTIFY_SELECTED)sendDataAvailableNotification();
  return true;
 }
 virtual void sendDataAvailableNotification(void) {
  leo_touch_inc(&notifyCalls);lastNotifyTail=writerTail();lastNotifyResult=UINT32_MAX;lastNotifyValid=false;
  mach_msg_header_t *message=(mach_msg_header_t *)notifyMsg;
  if(!message || !message->msgh_remote_port){leo_touch_inc(&notifyNoPort);return;}
  lastEnqueueFlags|=LEO_TOUCH_UP_NOTIFY_PORT;
  kern_return_t result=mach_msg_send_from_kernel_proper(message,message->msgh_size);
  lastNotifyResult=(UInt32)result;lastNotifyValid=true;
  if(result==MACH_MSG_SUCCESS){leo_touch_inc(&notifyOK);lastEnqueueFlags|=LEO_TOUCH_UP_NOTIFY_OK;}
  else if(result==MACH_SEND_TIMED_OUT){leo_touch_inc(&notifyBusy);lastEnqueueFlags|=LEO_TOUCH_UP_NOTIFY_BUSY;}
  else{leo_touch_inc(&notifyErrors);lastEnqueueFlags|=LEO_TOUCH_UP_NOTIFY_ERROR;}
 }
 void snapshot(LeoTouchObservation *out) const {
  out->notify_calls=notifyCalls;out->notify_ok=notifyOK;out->notify_busy=notifyBusy;
  out->notify_errors=notifyErrors;out->notify_no_port=notifyNoPort;
  out->last_notify_result=lastNotifyResult;out->last_notify_tail=lastNotifyTail;
  if(portPresent())out->observation_flags|=LEO_TOUCH_OBS_PORT_PRESENT;
  if(lastNotifyValid)out->observation_flags|=LEO_TOUCH_OBS_NOTIFY_RESULT_VALID;
  if(notifyCalls==UINT32_MAX || notifyOK==UINT32_MAX || notifyBusy==UINT32_MAX ||
   notifyErrors==UINT32_MAX || notifyNoPort==UINT32_MAX)out->observation_flags|=LEO_TOUCH_OBS_COUNTER_SATURATED;
 }
};
class IOS7LeoHIDTouchClient;
/* Matching class name is the original userspace IOServiceMatching contract. */
class IOHIDEventService : public IOService {
 OSDeclareDefaultStructors(IOHIDEventService)
 IOMemoryDescriptor *device[LEO_TOUCH_REGIONS],*queueMemory;
 IOMemoryMap *mapping[LEO_TOUCH_REGIONS];
 volatile uint8_t *registers[LEO_TOUCH_REGIONS];
 IOLock *lock;
 IOWorkLoop *loop;
 IOTimerEventSource *timer;
 IOS7LeoHIDQueue *queue;
 IOS7LeoHIDTouchClient *consumer;
 LeoTouchState state;
 LeoHIDPacket pendingPacket,lastPacket;
 bool opened,pending,previousDown,lastValid;
 unsigned packetLogs,eventLogs,queueErrors,transferLogs,readerProgress,readerLogs,observedHead;
 unsigned initValid,published,clientPID,mapCalls,notificationCalls,enqueued,downEvents,moveEvents,upEvents,queueFailures;
 unsigned queueEpoch,lastUpEpoch,lastUpEnqueued,lastUpTail,lastUpSequence,lastUpX,lastUpY,lastUpNotifyFlags;
 uint64_t lastUpTimestamp,serviceSender;
 int initResult;
 static uint32_t readRegister(void *,uint32_t,uint32_t);
 static void writeRegister(void *,uint32_t,uint32_t,uint32_t);
 static uint64_t nowNanoseconds(void *);
 static int delayMicroseconds(void *,uint32_t);
 static void tick(OSObject *,IOTimerEventSource *);
 void poll(void);
 bool publishProperties(void);
 void commitPacket(void);
 void closeReader(void);
 void publishSnapshot(void);
public:
 virtual bool init(OSDictionary *dictionary=0);
 virtual bool start(IOService *provider);
 virtual void free(void);
 virtual IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **);
 bool claim(IOS7LeoHIDTouchClient *);
 void unclaim(IOS7LeoHIDTouchClient *);
 IOReturn method(uint32_t,IOExternalMethodArguments *);
 IOReturn setPort(IOS7LeoHIDTouchClient *,mach_port_t,UInt32);
 IOReturn mapQueue(UInt32,IOOptionBits *,IOMemoryDescriptor **);
 void notePublished(void);
};
class IOS7LeoHIDTouchClient : public IOUserClient {
 OSDeclareDefaultStructors(IOS7LeoHIDTouchClient)
 IOHIDEventService *service;
 mach_port_t notification;
 IOLock *clientLock;
public:
 virtual bool initWithTask(task_t,void *,UInt32,OSDictionary *);
 virtual bool start(IOService *);
 virtual void free(void);
 virtual IOReturn clientClose(void);
 virtual IOService *getService(void);
 /* Both original ABI slots exist in this XNU. MIG dispatch uses the64-bit slot. */
 virtual IOReturn registerNotificationPort(mach_port_t,UInt32,io_user_reference_t);
 virtual IOReturn registerNotificationPort(mach_port_t,UInt32,UInt32);
 virtual IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **);
 virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,
  IOExternalMethodDispatch *,OSObject *,void *);
};
OSDefineMetaClassAndStructors(IOS7LeoHIDQueue,IOSharedDataQueue)
OSDefineMetaClassAndStructors(IOHIDEventService,IOService)
OSDefineMetaClassAndStructors(IOS7LeoHIDTouchClient,IOUserClient)

bool IOHIDEventService::init(OSDictionary *dictionary)
{
 if(!IOService::init(dictionary))return false;
 bzero(device,sizeof(device));bzero(mapping,sizeof(mapping));bzero(registers,sizeof(registers));
 queueMemory=0;lock=0;loop=0;timer=0;queue=0;consumer=0;
 bzero(&state,sizeof(state));bzero(&pendingPacket,sizeof(pendingPacket));bzero(&lastPacket,sizeof(lastPacket));
 opened=pending=previousDown=lastValid=false;
 packetLogs=eventLogs=queueErrors=transferLogs=readerProgress=readerLogs=observedHead=0;
 initValid=published=clientPID=mapCalls=notificationCalls=enqueued=downEvents=moveEvents=upEvents=queueFailures=0;
 queueEpoch=lastUpEpoch=lastUpEnqueued=lastUpTail=lastUpSequence=lastUpX=lastUpY=lastUpNotifyFlags=0;
 /* IORegistryEntry assigns its ID on attachToParent, after init. */
 lastUpTimestamp=0;serviceSender=0;
 initResult=LEO_TOUCH_NOT_READY;
 return true;
}
void IOHIDEventService::publishSnapshot(void)
{
 if(!OSCompareAndSwap(0,1,&leo_touch_observer_guard)){leo_touch_observer_lost();return;}
 serviceSender=getRegistryEntryID();
 LeoTouchObservation v={};v.version=IOS7_LEO_TOUCH_OBSERVATION_VERSION;v.known=1;
 v.init_valid=initValid;v.init_ready=state.ready;v.init_result=initResult;
 v.init_stage=state.stage;v.id_word=state.id_word;v.published=published;
 v.client_pid=clientPID;v.opened=opened;v.map_calls=mapCalls;v.notification_calls=notificationCalls;
 v.real_reports=state.packets;v.enqueued=enqueued;v.down_events=downEvents;v.move_events=moveEvents;v.up_events=upEvents;
 v.queue_failures=queueFailures;v.reader_progress=readerProgress;
 v.reader_head=queue?queue->readerHead():0;v.writer_tail=queue?queue->writerTail():0;v.pending=pending;
 v.last_sequence=state.sequence;v.last_x=state.last_x;v.last_y=state.last_y;v.last_down=(state.last_contacts!=0);
 v.transport_errors=state.errors;
 v.queue_epoch=queueEpoch;v.last_up_epoch=lastUpEpoch;v.last_up_enqueued=lastUpEnqueued;
 v.last_up_tail=lastUpTail;v.last_up_sequence=lastUpSequence;v.last_up_x=lastUpX;v.last_up_y=lastUpY;
 v.last_up_notify_flags=lastUpNotifyFlags;v.last_up_timestamp=lastUpTimestamp;v.service_sender=serviceSender;
 v.last_notify_result=UINT32_MAX;if(queue)queue->snapshot(&v);
 if(queueEpoch==UINT32_MAX || enqueued==UINT32_MAX || downEvents==UINT32_MAX || moveEvents==UINT32_MAX ||
  upEvents==UINT32_MAX || queueFailures==UINT32_MAX || readerProgress==UINT32_MAX || mapCalls==UINT32_MAX ||
  notificationCalls==UINT32_MAX || state.packets==UINT32_MAX || state.sequence==UINT32_MAX || state.errors==UINT32_MAX)
  v.observation_flags|=LEO_TOUCH_OBS_COUNTER_SATURATED;
 leo_touch_observer_state=v;
 OSMemoryBarrier();leo_touch_observer_guard=0;
}
void IOHIDEventService::notePublished(void)
{
 IOLockLock(lock);published=1;publishSnapshot();IOLockUnlock(lock);
}
uint32_t IOHIDEventService::readRegister(void *cookie,uint32_t region,uint32_t offset)
{
 IOHIDEventService *s=(IOHIDEventService *)cookie;
 if(!s || region>=LEO_TOUCH_REGIONS || offset>=4096 || (offset&3U) || !s->registers[region])return UINT32_MAX;
 __asm__ volatile("dsb sy" ::: "memory");
 uint32_t value=*(volatile uint32_t *)(s->registers[region]+offset);
 __asm__ volatile("dsb sy" ::: "memory");return value;
}
void IOHIDEventService::writeRegister(void *cookie,uint32_t region,uint32_t offset,uint32_t value)
{
 IOHIDEventService *s=(IOHIDEventService *)cookie;
 if(!s || region>=LEO_TOUCH_REGIONS || offset>=4096 || (offset&3U) || !s->registers[region])return;
 *(volatile uint32_t *)(s->registers[region]+offset)=value;
 __asm__ volatile("dsb sy" ::: "memory");
}
uint64_t IOHIDEventService::nowNanoseconds(void *)
{
 uint64_t now=0;(absolutetime_to_nanoseconds)(mach_absolute_time(),&now);return now;
}
int IOHIDEventService::delayMicroseconds(void *,uint32_t microseconds)
{
 if(microseconds>1000U)IOSleep(microseconds/1000U+(microseconds%1000U!=0));
 else IODelay(microseconds);
 return 1;
}
bool IOHIDEventService::publishProperties(void)
{
 setName("IOS7LeoType2ATouch");setProperty("HIDServiceSupport",true);
 setProperty("IOS7LabBackend","HTC Leo physical Type2A I2C native HID queue");
 setProperty("PrimaryUsagePage",13,32);setProperty("PrimaryUsage",4,32);
 setProperty("Built-In",true);setProperty("DisplayIntegrated",true);
 setProperty("Transport","I2C");setProperty("Product","HTC Leo Type2A touch");
 setProperty("Manufacturer","HD2 legacy lab");setProperty("VendorID",(unsigned long long)0,32);
 setProperty("ProductID",1,32);setProperty("ReportInterval",20000,32);setProperty("QueueSize",4096,32);
 OSDictionary *pair=OSDictionary::withCapacity(2);OSArray *pairs=OSArray::withCapacity(1);
 OSNumber *page=OSNumber::withNumber(13,32),*usage=OSNumber::withNumber(4,32);
 if(!pair || !pairs || !page || !usage){if(pair)pair->release();if(pairs)pairs->release();if(page)page->release();if(usage)usage->release();return false;}
 pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",usage);
 pairs->setObject(pair);setProperty("DeviceUsagePairs",pairs);
 page->release();usage->release();pair->release();pairs->release();
 OSDictionary *plugins=OSDictionary::withCapacity(4);
 OSString *path=OSString::withCString("IOHIDFamily.kext/PlugIns/IOHIDLib.plugin");
 if(!plugins || !path){if(plugins)plugins->release();if(path)path->release();return false;}
 plugins->setObject("7DDEECA8-A7B4-11DA-8A0E-0014519758EF",path);
 plugins->setObject("FA12FA38-6F1A-11D4-BA0C-0005028F18D5",path);
 plugins->setObject("0516B563-B15B-11DA-96EB-0014519758EF",path);
 plugins->setObject("40A57A4E-26A0-11D8-9295-000A958A2C78",path);
 setProperty("IOCFPlugInTypes",plugins);path->release();plugins->release();return true;
}
bool IOHIDEventService::start(IOService *provider)
{
 if(!IOService::start(provider))return false;
 lock=IOLockAlloc();queue=new IOS7LeoHIDQueue;
 if(!lock || !queue || !queue->initWithCapacity(4096))return false;
 queueMemory=queue->getMemoryDescriptor();if(!queueMemory)return false;
 static const uint32_t physical[LEO_TOUCH_REGIONS]={0xa9900000U,0xa8600000U,0xa9000000U,0xa9100000U};
 for(unsigned n=0;n<LEO_TOUCH_REGIONS;n++){
  device[n]=IOMemoryDescriptor::withPhysicalAddress(physical[n],4096,kIODirectionInOut);
  mapping[n]=device[n]?device[n]->map(kIOMapInhibitCache):0;
  if(!mapping[n] || !mapping[n]->getAddress()){
   IOLog("IOS7LEO TOUCH mapping-failed region=%u; no service published\n",n);return false;
  }
  registers[n]=(volatile uint8_t *)mapping[n]->getAddress();
 }
 LeoTouchIO io={this,readRegister,writeRegister,nowNanoseconds,delayMicroseconds,4096};
 int result=leo_touch_transport_init(&state,&io);
 initValid=1;initResult=result;publishSnapshot();
 IOLog("IOS7LEO TOUCH init result=%d stage=%u id=%08x diagnostic=%08x status=%08x actual-controller-only\n",
  result,state.stage,state.id_word,state.last_diagnostic,state.last_status);
 if(result!=LEO_TOUCH_OK || !state.ready)return false;
 loop=IOWorkLoop::workLoop();timer=IOTimerEventSource::timerEventSource(this,tick);
 if(!loop || !timer || loop->addEventSource(timer)!=kIOReturnSuccess)return false;
 if(!publishProperties())return false;
 if(timer->setTimeoutMS(20)!=kIOReturnSuccess)return false;
 IOLog("IOS7LEO TOUCH service-ready usage13/4 physical480x800 native204B queue4096; not-gesture-proof\n");
 return true;
}
void IOHIDEventService::free(void)
{
 if(timer)timer->cancelTimeout();if(loop && timer)loop->removeEventSource(timer);
 if(timer)timer->release();if(loop)loop->release();
 if(queueMemory)queueMemory->release();if(queue)queue->release();
 for(unsigned n=0;n<LEO_TOUCH_REGIONS;n++){
  if(mapping[n])mapping[n]->release();if(device[n])device[n]->release();
 }
 if(lock)IOLockFree(lock);IOService::free();
}
void IOHIDEventService::tick(OSObject *owner,IOTimerEventSource *source)
{
 IOHIDEventService *service=OSDynamicCast(IOHIDEventService,owner);
 if(service)service->poll();
 IOReturn result=source->setTimeoutMS(20);
 static unsigned logs;if(result!=kIOReturnSuccess && logs<1U){++logs;IOLog("IOS7LEO TOUCH rearm-error=%x; no further polling claimed\n",result);}
}
void IOHIDEventService::commitPacket(void)
{
 bool down=(pendingPacket.finger.options&0x20000U)!=0;
 leo_touch_inc(&enqueued);if(down)leo_touch_inc(previousDown?&moveEvents:&downEvents);else leo_touch_inc(&upEvents);
 if(!down){
  lastUpEpoch=queueEpoch;lastUpEnqueued=enqueued;lastUpTail=queue->writerTail();
  lastUpSequence=state.sequence;lastUpX=state.last_x;lastUpY=state.last_y;
  lastUpTimestamp=pendingPacket.header.timeStamp;lastUpNotifyFlags=queue->enqueueNotificationFlags();
 }
 if(eventLogs<32U){
  ++eventLogs;
  IOLog("IOS7LEO TOUCH enqueued seq=%u kind=%s x=%u y=%u down=%u timestamp=%llu; queue-enqueue-not-UI-consumption\n",
   state.sequence,down?(previousDown?"move":"down"):"up",(unsigned)state.last_x,(unsigned)state.last_y,
   (unsigned)down,(unsigned long long)pendingPacket.header.timeStamp);
 }
 previousDown=down;lastPacket=pendingPacket;lastValid=true;pending=false;
 publishSnapshot();
}
void IOHIDEventService::poll(void)
{
 IOLockLock(lock);
 UInt32 head=queue->readerHead();
 if(head!=observedHead){
  leo_touch_inc(&readerProgress);
  if(readerLogs<8U){++readerLogs;IOLog("IOS7LEO TOUCH reader-head head=%u tail=%u; queue-progress-only\n",head,queue->writerTail());}
  observedHead=head;
 }
 /* Refresh actual shared RAM frontiers on every normal20ms poll, including
  * no new input/read errors. Observation never initiates hardware reads. */
 publishSnapshot();
 if(pending && opened){
  if(!queue->sane() || !queue->enqueue(&pendingPacket,sizeof(pendingPacket))){leo_touch_inc(&queueFailures);publishSnapshot();IOLockUnlock(lock);return;}
  commitPacket();
 }
 LeoTouchReport report;int result=leo_touch_transport_poll(&state,&report);
 if(result<0){
  publishSnapshot();
  if(transferLogs<4U){++transferLogs;IOLog("IOS7LEO TOUCH poll-error=%d stage=%u status=%08x diagnostic=%08x; no-synthetic-up\n",result,state.stage,state.last_status,state.last_diagnostic);}
  IOLockUnlock(lock);return;
 }
 if(result==0){IOLockUnlock(lock);return;}
 publishSnapshot();
 if(packetLogs<32U){
  ++packetLogs;IOLog("IOS7LEO TOUCH real-report seq=%u x=%u y=%u down=%u contacts=%u raw=%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
   report.sequence,(unsigned)report.x,(unsigned)report.y,(unsigned)report.down,(unsigned)report.contacts,
   report.raw[0],report.raw[1],report.raw[2],report.raw[3],report.raw[4],report.raw[5],report.raw[6],report.raw[7],report.raw[8]);
 }
 if(!opened){previousDown=false;IOLockUnlock(lock);return;}
 if(!report.down && !previousDown){IOLockUnlock(lock);return;}
 if(!leo_touch_make_hid_packet(report.x,report.y,report.down,previousDown,
  mach_absolute_time(),getRegistryEntryID(),&pendingPacket)){IOLockUnlock(lock);return;}
 if(!queue->sane() || !queue->enqueue(&pendingPacket,sizeof(pendingPacket))){
  pending=true;
  leo_touch_inc(&queueFailures);publishSnapshot();
  if(queueErrors<4U){++queueErrors;IOLog("IOS7LEO TOUCH queue-full-or-invalid; retained pending actual contact, no new hardware read\n");}
 }else commitPacket();
 IOLockUnlock(lock);
}
void IOHIDEventService::closeReader(void)
{
 opened=pending=previousDown=lastValid=false;
 leo_touch_inc(&queueEpoch);
 queue->discardAll();observedHead=0;
 publishSnapshot();
}
bool IOHIDEventService::claim(IOS7LeoHIDTouchClient *client)
{
 IOLockLock(lock);bool accepted=client && !consumer;if(accepted){consumer=client;clientPID=(unsigned)proc_selfpid();publishSnapshot();}IOLockUnlock(lock);return accepted;
}
void IOHIDEventService::unclaim(IOS7LeoHIDTouchClient *client)
{
 IOLockLock(lock);if(consumer==client){closeReader();consumer=0;clientPID=0;queue->setNotificationPort(MACH_PORT_NULL);publishSnapshot();}IOLockUnlock(lock);
}
IOReturn IOHIDEventService::setPort(IOS7LeoHIDTouchClient *client,mach_port_t port,UInt32 type)
{
 if(type)return kIOReturnUnsupported;
 IOLockLock(lock);
 if(consumer!=client){IOLockUnlock(lock);return kIOReturnNotAttached;}
 queue->setNotificationPort(port);leo_touch_inc(&notificationCalls);publishSnapshot();IOLockUnlock(lock);
 static unsigned logs;if(logs<8U){++logs;IOLog("IOS7LEO TOUCH notification-port type0 registered; not-event-delivery-proof\n");}
 return kIOReturnSuccess;
}
IOReturn IOHIDEventService::mapQueue(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
 if(type || !options || !memory || !queueMemory)return kIOReturnBadArgument;
 queueMemory->retain();*options=0;*memory=queueMemory;
 IOLockLock(lock);leo_touch_inc(&mapCalls);publishSnapshot();IOLockUnlock(lock);
 static unsigned logs;if(logs<8U){++logs;IOLog("IOS7LEO TOUCH queue-map type0 bytes=%lu\n",(unsigned long)queueMemory->getLength());}
 return kIOReturnSuccess;
}
IOReturn IOHIDEventService::method(uint32_t selector,IOExternalMethodArguments *args)
{
 if(!args || args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
 static unsigned logs;if(logs<12U){++logs;IOLog("IOS7LEO TOUCH method=%u sin=%u stin=%u sout=%u stout=%u\n",selector,args->scalarInputCount,args->structureInputSize,args->scalarOutputCount,args->structureOutputSize);}
 if((selector==0 || selector==1) && args->scalarInputCount==1 && args->scalarInput &&
  !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize){
  if(args->scalarInput[0])return kIOReturnUnsupported;
  IOLockLock(lock);
  if(selector==0){if(!opened){closeReader();opened=true;}}
  else closeReader();
  publishSnapshot();
  IOLockUnlock(lock);
  static unsigned openLogs;if(openLogs<8U){++openLogs;IOLog("IOS7LEO TOUCH reader-%s selector=%u\n",selector==0?"open":"close",selector);}
  return kIOReturnSuccess;
 }
 if(selector==2 && args->scalarInputCount==2 && args->scalarInput && !args->structureInputSize &&
  !args->scalarOutputCount && args->structureOutput && args->structureOutputSize>=sizeof(lastPacket)){
  if(args->scalarInput[0]!=11 || args->scalarInput[1])return kIOReturnUnsupported;
  IOLockLock(lock);bool available=lastValid;
  if(available){memcpy(args->structureOutput,&lastPacket,sizeof(lastPacket));args->structureOutputSize=sizeof(lastPacket);}
  IOLockUnlock(lock);return available?kIOReturnSuccess:kIOReturnNotFound;
 }
 return kIOReturnUnsupported;
}
bool IOS7LeoHIDTouchClient::initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties)
{
 service=0;notification=MACH_PORT_NULL;clientLock=IOLockAlloc();
 return clientLock && IOUserClient::initWithTask(task,security,type,properties);
}
bool IOS7LeoHIDTouchClient::start(IOService *provider)
{
 if(!IOUserClient::start(provider))return false;
 IOHIDEventService *candidate=OSDynamicCast(IOHIDEventService,provider);
 if(!candidate)return false;
 IOLockLock(clientLock);bool accepted=candidate->claim(this);
 if(accepted)service=candidate;IOLockUnlock(clientLock);return accepted;
}
IOReturn IOS7LeoHIDTouchClient::clientClose(void)
{
 IOLockLock(clientLock);
 if(service){service->unclaim(this);service=0;}
 if(notification){releaseNotificationPort(notification);notification=MACH_PORT_NULL;}
 IOLockUnlock(clientLock);
 terminate();return kIOReturnSuccess;
}
void IOS7LeoHIDTouchClient::free(void)
{
 if(clientLock){
  IOLockLock(clientLock);if(service){service->unclaim(this);service=0;}
  if(notification){releaseNotificationPort(notification);notification=MACH_PORT_NULL;}
  IOLockUnlock(clientLock);IOLockFree(clientLock);clientLock=0;
 }
 IOUserClient::free();
}
IOService *IOS7LeoHIDTouchClient::getService(void)
{
 IOLockLock(clientLock);IOService *result=service;IOLockUnlock(clientLock);return result;
}
IOReturn IOS7LeoHIDTouchClient::registerNotificationPort(mach_port_t port,UInt32 type,io_user_reference_t)
{
 IOLockLock(clientLock);
 if(!service){IOLockUnlock(clientLock);return kIOReturnNotAttached;}
 IOReturn result=service->setPort(this,port,type);
 if(result==kIOReturnSuccess){if(notification)releaseNotificationPort(notification);notification=port;}
 IOLockUnlock(clientLock);
 return result;
}
IOReturn IOS7LeoHIDTouchClient::registerNotificationPort(mach_port_t port,UInt32 type,UInt32 refCon)
{
 return registerNotificationPort(port,type,(io_user_reference_t)refCon);
}
IOReturn IOS7LeoHIDTouchClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
 IOLockLock(clientLock);IOReturn result=service?service->mapQueue(type,options,memory):kIOReturnNotAttached;IOLockUnlock(clientLock);return result;
}
IOReturn IOS7LeoHIDTouchClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,
 IOExternalMethodDispatch *,OSObject *,void *)
{
 IOLockLock(clientLock);IOReturn result=service?service->method(selector,args):kIOReturnNotAttached;IOLockUnlock(clientLock);return result;
}
IOReturn IOHIDEventService::newUserClient(task_t task,void *security,UInt32 type,
 OSDictionary *properties,IOUserClient **handler)
{
 static unsigned logs;if(logs<8U){++logs;IOLog("IOS7LEO TOUCH client type=%u pid=%d\n",type,proc_selfpid());}
 if(type || !handler)return kIOReturnBadArgument;*handler=0;
 IOS7LeoHIDTouchClient *client=new IOS7LeoHIDTouchClient;if(!client)return kIOReturnNoMemory;
 if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnNoMemory;}
 if(!client->attach(this)){client->release();return kIOReturnError;}
 if(!client->start(this)){client->detach(this);client->release();return kIOReturnExclusiveAccess;}
 *handler=client;return kIOReturnSuccess;
}
void ios7leo_register_hid_touch(IOService *platform)
{
 IOHIDEventService *service=new IOHIDEventService;if(!service)return;
 bool attached=service->init() && service->attach(platform);
 if(attached && service->start(platform)){
  service->registerService();service->notePublished();IOLog("IOS7LEO TOUCH published actual Type2A service\n");
 }else if(attached)service->detach(platform);
 service->release();
}
#endif
