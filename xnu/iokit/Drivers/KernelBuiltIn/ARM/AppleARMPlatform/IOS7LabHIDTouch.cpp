#if !defined(BOARD_CONFIG_QSD8250_LEO)
/* Native event-service transport for the RealView PL050 mouse.
 * It implements the original iOS7 IOHIDLib service protocol and emits only
 * actual button/motion packets. No userland gesture or foreground app helper. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
/* kern/queue.h's C macro must not rename IODataQueue's C++ virtual method. */
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
#include "IOS7LabHIDWire.h"
#include "IOS7LabPS2StreamPoll.h"
extern "C" {
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>
extern int proc_selfpid(void);
}

#if BOARD_CONFIG_ARMPBA8
class IOS7LabHIDQueue : public IOSharedDataQueue {
    OSDeclareDefaultStructors(IOS7LabHIDQueue)
public:
    UInt32 readerHead(void) const { return dataQueue ? dataQueue->head : 0; }
    UInt32 writerTail(void) const { return dataQueue ? dataQueue->tail : 0; }
    bool sane(void) const {
        return dataQueue && dataQueue->queueSize==4096 &&
            dataQueue->head<=4096 && dataQueue->tail<=4096 &&
            !(dataQueue->head&3) && !(dataQueue->tail&3);
    }
};
class IOS7LabHIDTouchClient;
class IOHIDEventService : public IOService {
    OSDeclareDefaultStructors(IOHIDEventService)
    IOMemoryDescriptor *device,*queueMemory;
    IOMemoryMap *mapping;
    IOLock *lock;
    volatile uint32_t *registers;
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    IOS7LabHIDQueue *queue;
    IOS7LabHIDTouchClient *consumer;
    IOS7LabPS2State state;
    IOS7LabPS2PollState pollState;
    unsigned packetLogs;
    IOS7LabHIDPacket pendingPacket,lastPacket;
    bool opened,pending,previousDown,lastValid;
    unsigned emitted,queueErrors,observedHead,readerProgress;
    bool sendCommand(uint8_t value);
    static void tick(OSObject *owner,IOTimerEventSource *source);
    void poll(void);
public:
    virtual bool init(OSDictionary *dictionary=0);
    virtual bool start(IOService *provider);
    virtual void free(void);
    virtual IOReturn newUserClient(task_t task,void *security,UInt32 type,
        OSDictionary *properties,IOUserClient **handler);
    bool claim(IOS7LabHIDTouchClient *client);
    void unclaim(IOS7LabHIDTouchClient *client);
    IOReturn method(uint32_t selector,IOExternalMethodArguments *args);
    IOReturn setPort(mach_port_t port,UInt32 type);
    IOReturn mapQueue(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory);
};
class IOS7LabHIDTouchClient : public IOUserClient {
    OSDeclareDefaultStructors(IOS7LabHIDTouchClient)
    IOHIDEventService *service;
    mach_port_t notification;
public:
    virtual bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties);
    virtual bool start(IOService *provider);
    virtual void free(void);
    virtual IOReturn clientClose(void);
    virtual IOService *getService(void) { return service; }
    virtual IOReturn registerNotificationPort(mach_port_t port,UInt32 type,UInt32 refCon);
    virtual IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory);
    virtual IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,
        IOExternalMethodDispatch *,OSObject *,void *);
};
OSDefineMetaClassAndStructors(IOS7LabHIDQueue,IOSharedDataQueue)
OSDefineMetaClassAndStructors(IOHIDEventService,IOService)
OSDefineMetaClassAndStructors(IOS7LabHIDTouchClient,IOUserClient)

bool IOHIDEventService::init(OSDictionary *dictionary)
{
    if(!IOService::init(dictionary))return false;
    device=queueMemory=0;mapping=0;lock=0;registers=0;loop=0;timer=0;queue=0;consumer=0;
    opened=pending=previousDown=lastValid=false;emitted=queueErrors=observedHead=readerProgress=0;
    bzero(&state,sizeof(state));bzero(&pollState,sizeof(pollState));packetLogs=0;
    bzero(&pendingPacket,sizeof(pendingPacket));bzero(&lastPacket,sizeof(lastPacket));
    return true;
}
bool IOHIDEventService::sendCommand(uint8_t value)
{
    for(unsigned i=0;i<100;++i){
        if(registers[1]&0x40){
            registers[2]=value;__sync_synchronize();
            for(unsigned j=0;j<100;++j){if(registers[1]&0x10)return (registers[2]&255)==0xfa;IOSleep(1);}
            return false;
        }
        IOSleep(1);
    }
    return false;
}
bool IOHIDEventService::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    lock=IOLockAlloc();queue=new IOS7LabHIDQueue;
    if(!lock || !queue || !queue->initWithCapacity(4096))return false;
    queueMemory=queue->getMemoryDescriptor();
    device=IOMemoryDescriptor::withPhysicalAddress(0x10007000,4096,kIODirectionInOut);
    mapping=device?device->map(kIOMapInhibitCache):0;
    if(!queueMemory || !mapping || !mapping->getAddress())return false;
    registers=(volatile uint32_t *)mapping->getAddress();
    if((registers[0xfe0/4]&255)!=0x50 || (registers[0xfe4/4]&255)!=0x10 ||
       (registers[0xfe8/4]&255)!=4 || (registers[0xfec/4]&255)!=0){registers=0;return false;}
    registers[3]=8;registers[0]=4;__sync_synchronize();
    for(unsigned n=0;n<32 && (registers[1]&0x10);++n){uint32_t discarded=registers[2];(void)discarded;}
    if(!sendCommand(0xf6)||!sendCommand(0xf4))return false;
    loop=IOWorkLoop::workLoop();
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!loop || !timer || loop->addEventSource(timer)!=kIOReturnSuccess)return false;
    setName("IOS7LabPL050Touch");
    setProperty("HIDServiceSupport",true);
    setProperty("IOS7LabBackend","RealView PL050 native HID queue");
    setProperty("PrimaryUsagePage",13,32);setProperty("PrimaryUsage",4,32);
    setProperty("Built-In",true);setProperty("DisplayIntegrated",true);
    setProperty("Transport","PS2");setProperty("Product","RealView PL050 touch adapter");
    setProperty("Manufacturer","HD2 legacy lab");setProperty("VendorID",(unsigned long long)0,32);setProperty("ProductID",1,32);
    setProperty("ReportInterval",8000,32);setProperty("QueueSize",4096,32);
    OSDictionary *pair=OSDictionary::withCapacity(2);
    OSArray *pairs=OSArray::withCapacity(1);
    OSNumber *page=OSNumber::withNumber(13,32),*usage=OSNumber::withNumber(4,32);
    if(!pair||!pairs||!page||!usage){if(pair)pair->release();if(pairs)pairs->release();if(page)page->release();if(usage)usage->release();return false;}
    pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",usage);
    pairs->setObject(pair);setProperty("DeviceUsagePairs",pairs);
    page->release();usage->release();pair->release();pairs->release();
    OSDictionary *plugins=OSDictionary::withCapacity(4);
    /* Original iOS7 IOCFPlugIn unconditionally prepends /System/Library/Extensions/. */
    OSString *path=OSString::withCString("IOHIDFamily.kext/PlugIns/IOHIDLib.plugin");
    if(!plugins || !path){if(plugins)plugins->release();if(path)path->release();return false;}
    /* Exact factory types from the original IOHIDLib Info.plist. */
    plugins->setObject("7DDEECA8-A7B4-11DA-8A0E-0014519758EF",path);
    plugins->setObject("FA12FA38-6F1A-11D4-BA0C-0005028F18D5",path);
    plugins->setObject("0516B563-B15B-11DA-96EB-0014519758EF",path);
    plugins->setObject("40A57A4E-26A0-11D8-9295-000A958A2C78",path);
    setProperty("IOCFPlugInTypes",plugins);path->release();plugins->release();
    timer->setTimeoutMS(8);
    IOLog("IOS7LAB native HID event service ready: physical PL050, usage13/4, queue4096\n");
    return true;
}
void IOHIDEventService::free(void)
{
    if(timer)timer->cancelTimeout();
    if(loop&&timer)loop->removeEventSource(timer);
    if(timer)timer->release();if(loop)loop->release();
    if(registers)registers[0]=0;
    if(queueMemory)queueMemory->release();if(queue)queue->release();
    if(mapping)mapping->release();if(device)device->release();if(lock)IOLockFree(lock);
    IOService::free();
}
void IOHIDEventService::tick(OSObject *owner,IOTimerEventSource *source)
{
    IOHIDEventService *service=OSDynamicCast(IOHIDEventService,owner);
    if(service)service->poll();
    source->setTimeoutMS(8);
}
void IOHIDEventService::poll(void)
{
    IOLockLock(lock);
    if(ios7lab_ps2_poll_tick(&pollState))
        IOLog("IOS7LAB PS2 AUX_POLL timeout; no further commands, retain framing\n");
    UInt32 currentHead=queue->readerHead();
    if(currentHead!=observedHead){
        if(readerProgress++<8)
            IOLog("IOS7LAB native HID reader advanced head=%u tail=%u\n",(unsigned)currentHead,(unsigned)queue->writerTail());
        observedHead=currentHead;
    }
    if(pending && opened){
        if(!queue->sane() || !queue->enqueue(&pendingPacket,sizeof(pendingPacket))){IOLockUnlock(lock);return;}
        previousDown=(pendingPacket.finger.options&0x20000)!=0;lastPacket=pendingPacket;lastValid=true;pending=false;
    }
    for(unsigned n=0;n<48 && (registers[1]&0x10);++n){
        uint8_t rawByte=(uint8_t)registers[2];
        unsigned countBefore=state.count,phaseBefore=pollState.phase,errorsBefore=state.errors;
        uint32_t xBefore=state.x,yBefore=state.y,buttonsBefore=state.buttons;
        unsigned decoded=ios7lab_ps2_poll_byte(&pollState,&state,rawByte,640,960);
        if(decoded==IOS7LAB_PS2_COMMAND_ACK && pollState.acknowledgements<=4)
            IOLog("IOS7LAB PS2 AUX_POLL ACK command=%u phase=%u count=%u\n",
                pollState.commands,pollState.phase,state.count);
        if((decoded==IOS7LAB_PS2_REAL_PACKET ||
            (countBefore==2 && !state.count && state.errors!=errorsBefore)) && packetLogs++<64)
            IOLog("IOS7LAB PS2 frame=%u raw=%02x,%02x,%02x delta=%d,%d phase=%u result=%u before=%u,%u,%u after=%u,%u,%u errors=%u\n",
                state.sequence,state.bytes[0],state.bytes[1],state.bytes[2],
                (int)state.bytes[1]-((state.bytes[0]&0x10)?256:0),
                (int)state.bytes[2]-((state.bytes[0]&0x20)?256:0),
                phaseBefore,decoded,xBefore,yBefore,buttonsBefore,state.x,state.y,state.buttons,state.errors);
        if(decoded!=IOS7LAB_PS2_REAL_PACKET)continue;
        bool down=(state.buttons&1)!=0;
        if(!opened){previousDown=false;continue;}
        if(!down && !previousDown)continue;
        if(!ios7lab_make_hid_packet(&state,previousDown,mach_absolute_time(),getRegistryEntryID(),&pendingPacket))continue;
        if(!queue->sane() || !queue->enqueue(&pendingPacket,sizeof(pendingPacket))){
            pending=true;
            if(queueErrors++<4)IOLog("IOS7LAB native HID queue stalled; retaining pending input\n");
            break;
        }
        if(down!=previousDown && emitted++<8)
            IOLog("IOS7LAB native HID queued seq=%u x=%u y=%u down=%u timestamp=%llu\n",state.sequence,state.x,state.y,(unsigned)down,(unsigned long long)pendingPacket.header.timeStamp);
        previousDown=down;lastPacket=pendingPacket;lastValid=true;
    }
    /* Standard AUX_POLL reads actual retained device displacement. Stream
     * mode remains enabled, preserving its queued button transitions. */
    if(!pending && (registers[1]&0x50)==0x40 &&
       ios7lab_ps2_poll_begin(&pollState,&state)){
        registers[2]=0xeb;__sync_synchronize();
        if(pollState.commands<=4)
            IOLog("IOS7LAB PS2 AUX_POLL request=%u decoder-count=%u\n",pollState.commands,state.count);
    }
    IOLockUnlock(lock);
}
bool IOHIDEventService::claim(IOS7LabHIDTouchClient *client)
{
    IOLockLock(lock);bool accepted=!consumer;
    if(accepted)consumer=client;
    IOLockUnlock(lock);return accepted;
}
void IOHIDEventService::unclaim(IOS7LabHIDTouchClient *client)
{
    IOLockLock(lock);
    if(consumer==client){opened=false;pending=false;previousDown=false;consumer=0;queue->setNotificationPort(MACH_PORT_NULL);}
    IOLockUnlock(lock);
}
IOReturn IOHIDEventService::setPort(mach_port_t port,UInt32 type)
{
    if(type)return kIOReturnUnsupported;
    IOLockLock(lock);queue->setNotificationPort(port);IOLockUnlock(lock);
    IOLog("IOS7LAB native HID notification port registered\n");return kIOReturnSuccess;
}
IOReturn IOHIDEventService::mapQueue(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(type || !options || !memory || !queueMemory)return kIOReturnBadArgument;
    queueMemory->retain();*options=0;*memory=queueMemory;
    IOLog("IOS7LAB native HID queue memory requested bytes=%lu\n",(unsigned long)queueMemory->getLength());
    return kIOReturnSuccess;
}
IOReturn IOHIDEventService::method(uint32_t selector,IOExternalMethodArguments *args)
{
    if(!args || args->structureInputDescriptor || args->structureOutputDescriptor)return kIOReturnBadArgument;
    static unsigned logs;
    if(logs++<12){
        IOLog("IOS7LAB native HID method=%u sin=%u stin=%u sout=%u stout=%u\n",selector,args->scalarInputCount,args->structureInputSize,args->scalarOutputCount,args->structureOutputSize);
        if(selector==2 && args->scalarInputCount==2 && args->scalarInput)
            IOLog("IOS7LAB native HID copy-event type=%u options=%u\n",(unsigned)args->scalarInput[0],(unsigned)args->scalarInput[1]);
    }
    if((selector==0 || selector==1) && args->scalarInputCount==1 && args->scalarInput &&
       !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize){
        if(args->scalarInput[0])return kIOReturnUnsupported;
        IOLockLock(lock);opened=selector==0;pending=false;previousDown=false;IOLockUnlock(lock);
        IOLog("IOS7LAB native HID reader %s\n",opened?"opened":"closed");return kIOReturnSuccess;
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
bool IOS7LabHIDTouchClient::initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties)
{
    service=0;notification=MACH_PORT_NULL;
    return IOUserClient::initWithTask(task,security,type,properties);
}
bool IOS7LabHIDTouchClient::start(IOService *provider)
{
    if(!IOUserClient::start(provider))return false;
    IOHIDEventService *candidate=OSDynamicCast(IOHIDEventService,provider);
    if(!candidate || !candidate->claim(this))return false;
    service=candidate;return true;
}
IOReturn IOS7LabHIDTouchClient::clientClose(void)
{
    if(service){service->unclaim(this);service=0;}
    if(notification){releaseNotificationPort(notification);notification=MACH_PORT_NULL;}
    terminate();return kIOReturnSuccess;
}
void IOS7LabHIDTouchClient::free(void)
{
    if(service)service->unclaim(this);
    if(notification)releaseNotificationPort(notification);
    IOUserClient::free();
}
IOReturn IOS7LabHIDTouchClient::registerNotificationPort(mach_port_t port,UInt32 type,UInt32)
{
    if(!service)return kIOReturnNotAttached;
    IOReturn result=service->setPort(port,type);
    if(result==kIOReturnSuccess){if(notification)releaseNotificationPort(notification);notification=port;}
    return result;
}
IOReturn IOS7LabHIDTouchClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    return service?service->mapQueue(type,options,memory):kIOReturnNotAttached;
}
IOReturn IOS7LabHIDTouchClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,
    IOExternalMethodDispatch *,OSObject *,void *)
{
    return service?service->method(selector,args):kIOReturnNotAttached;
}
IOReturn IOHIDEventService::newUserClient(task_t task,void *security,UInt32 type,
    OSDictionary *properties,IOUserClient **handler)
{
    IOLog("IOS7LAB native HID user client type=%u caller-pid=%d\n",(unsigned)type,proc_selfpid());
    if(type || !handler)return kIOReturnBadArgument;
    *handler=0;
    IOS7LabHIDTouchClient *client=new IOS7LabHIDTouchClient;
    if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnNoMemory;}
    if(!client->attach(this)){client->release();return kIOReturnError;}
    if(!client->start(this)){client->detach(this);client->release();return kIOReturnExclusiveAccess;}
    *handler=client;return kIOReturnSuccess;
}
void ios7lab_register_hid_touch(IOService *platform)
{
    int enabled=0;
    if(!PE_parse_boot_argn("ios7lab_vmabi",&enabled,sizeof(enabled)) || enabled!=1)return;
    IOHIDEventService *service=new IOHIDEventService;
    if(!service)return;
    bool attached=service->init()&&service->attach(platform);
    if(attached&&service->start(platform))service->registerService();
    else if(attached)service->detach(platform);
    service->release();
}
#endif

#endif /* RealView transport is not active in Leo entry-only image. */
