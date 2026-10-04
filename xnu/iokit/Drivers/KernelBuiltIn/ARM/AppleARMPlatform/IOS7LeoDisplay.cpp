#if defined(BOARD_CONFIG_QSD8250_LEO)
/* Leo adaptation of proven original iOS7 CPU surface/discovery wire. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/OSAtomic.h>
#include "IOS7LabSurfaceGeometry.h"
#include "IOS7LeoDisplayBackend.h"
#include "IOS7LeoDisplayObservation.h"
#include "IOS7LeoGraphicsState.h"
#include <mach/vm_param.h>
extern "C" {
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
extern void task_reference(task_t);
extern void task_deallocate(task_t);
extern unsigned int vm_page_free_count,vm_page_free_reserved,vm_page_free_target;
/* Exact naked-send-right API: osfmk/ipc/ipc_port.h442 / ipc_port.c1107.
 * mach/port.h makes mach_port_t and ipc_port_t the same kernel pointer type. */
extern ipc_port_t ipc_port_copy_send(ipc_port_t);
extern void *get_bsdtask_info(task_t);
struct proc;
extern int proc_pid(struct proc *);
}

struct LeoGraphicsCall {
    uint64_t transaction;
    uint32_t surface,reason,mismatch;
    bool backend;
};
enum { LEO_VSYNC_PORT=1,LEO_VSYNC_CALLBACK=2,LEO_VSYNC_EMIT=3,LEO_VSYNC_CLOSE=4 };
struct LeoVsyncAction {
    uint32_t operation;
    mach_port_t port;
    UInt32 type;
    io_user_reference_t refcon,callback;
    IOTimerEventSource *sender;
};
static volatile UInt32 leo_graphics_guard,leo_graphics_losses,leo_graphics_unknown;
static volatile UInt32 leo_graphics_client_ids;
static LeoGraphicsState leo_graphics_state;
static LeoGraphicsEvent leo_graphics_events[LEO_GRAPHICS_EVENT_CAPACITY];
static void leo_graphics_inc(uint32_t *value){if(*value!=UINT32_MAX)++*value;}
static void leo_graphics_drop(void)
{
    for(unsigned i=0;i<4;i++){
        UInt32 old=leo_graphics_losses;
        if(old==UINT32_MAX)return;
        if(OSCompareAndSwap(old,old+1U,&leo_graphics_losses))return;
    }
    (void)OSCompareAndSwap(0U,1U,&leo_graphics_unknown);
}
static bool leo_graphics_try(void)
{
    if(OSCompareAndSwap(0U,1U,&leo_graphics_guard))return true;
    leo_graphics_drop();return false;
}
static void leo_graphics_unlock(void){OSMemoryBarrier();leo_graphics_guard=0;}
static void leo_graphics_attempt(uint32_t selector)
{
    if(!leo_graphics_try())return;
    uint32_t *counter=selector==4?&leo_graphics_state.selector4_calls:
        selector==5?&leo_graphics_state.selector5_calls:
        selector==6?&leo_graphics_state.selector6_calls:&leo_graphics_state.selector9_calls;
    leo_graphics_inc(counter);leo_graphics_unlock();
}
static void leo_graphics_open(uint32_t client,uint32_t pid)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.clients);
    leo_graphics_state.latest_open_client=client;
    leo_graphics_state.latest_open_pid=pid;
    leo_graphics_unlock();
}
static void leo_graphics_prereq_attempt(uint32_t client,uint32_t pid,uint32_t selector)
{
    if(!leo_graphics_try())return;
    uint32_t *counter=selector==3?&leo_graphics_state.selector3_calls:
        selector==8?&leo_graphics_state.selector8_calls:
        &leo_graphics_state.selector18_calls;
    leo_graphics_inc(counter);
    leo_graphics_state.prereq_last_client=client;
    leo_graphics_state.prereq_last_pid=pid;
    leo_graphics_state.prereq_last_selector=selector;
    leo_graphics_state.prereq_last_result=UINT32_MAX;
    leo_graphics_unlock();
}
static void leo_graphics_prereq_result(uint32_t client,uint32_t pid,uint32_t selector,
    IOReturn result)
{
    if(!leo_graphics_try())return;
    if(result==kIOReturnSuccess){
        uint32_t *counter=selector==3?&leo_graphics_state.selector3_successes:
            selector==8?&leo_graphics_state.selector8_successes:
            &leo_graphics_state.selector18_successes;
        leo_graphics_inc(counter);
    }
    leo_graphics_state.prereq_last_client=client;
    leo_graphics_state.prereq_last_pid=pid;
    leo_graphics_state.prereq_last_selector=selector;
    leo_graphics_state.prereq_last_result=(uint32_t)result;
    leo_graphics_unlock();
}
static void leo_graphics_port_entry(uint32_t client,uint32_t pid,UInt32 type,bool valid)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.port_entries);
    leo_graphics_state.port_entry_client=client;
    leo_graphics_state.port_entry_pid=pid;
    leo_graphics_state.port_entry_type=type;
    leo_graphics_state.port_entry_valid=valid?1U:0U;
    leo_graphics_state.port_last_return=UINT32_MAX;
    leo_graphics_unlock();
}
static void leo_graphics_port_preflight(IOReturn result)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.port_preflight_rejects);
    leo_graphics_inc(&leo_graphics_state.port_returns);
    leo_graphics_state.port_last_return=(uint32_t)result;
    leo_graphics_unlock();
}
static void leo_graphics_port_gate_entry(void)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.port_gate_entries);
    leo_graphics_unlock();
}
static void leo_graphics_port_return(IOReturn result)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.port_returns);
    leo_graphics_state.port_last_return=(uint32_t)result;
    leo_graphics_unlock();
}
static void leo_graphics_note(const LeoGraphicsEvent &event,bool backend,bool published)
{
    if(!leo_graphics_try())return;
    LeoGraphicsState &s=leo_graphics_state;
    s.last_client=event.client;s.owner_pid=event.owner_pid;
    s.requested=event.requested;s.enabled=event.enabled;s.closed=event.closed;
    s.port_present=event.port_present;s.callback_present=event.callback_present;
    s.generation_lo=event.generation_lo;s.generation_hi=event.generation_hi;
    if(event.kind==LEO_GRAPHICS_EVENT_METHOD){
        s.last_selector=event.selector;s.last_result=event.result;s.last_reason=event.reason;
        if(event.selector==4){s.selector4_result=event.result;s.selector4_reason=event.reason;if(event.result)leo_graphics_inc(&s.selector4_rejects);}
        if(event.selector==5){s.selector5_result=event.result;s.selector5_reason=event.reason;if(event.result)leo_graphics_inc(&s.selector5_rejects);
            s.last_transaction_lo=event.transaction_lo;s.last_transaction_hi=event.transaction_hi;s.last_surface=event.surface;s.last_mismatch=event.mismatch;
            if(backend){leo_graphics_inc(&s.selector5_backend_calls);}
            if(backend&&!event.result){leo_graphics_inc(&s.selector5_completed);}
        }
        if(event.selector==6){s.selector6_result=event.result;s.selector6_reason=event.reason;if(event.result)leo_graphics_inc(&s.selector6_rejects);s.last_wait_lo=event.transaction_lo;s.last_wait_hi=event.transaction_hi;}
        if(event.selector==9){s.selector9_result=event.result;s.selector9_reason=event.reason;if(event.result)leo_graphics_inc(&s.selector9_rejects);}
    }else if(event.kind==LEO_GRAPHICS_EVENT_PORT){
        leo_graphics_inc(&s.port_calls);if(event.result)leo_graphics_inc(&s.port_rejects);
    }else if(event.kind==LEO_GRAPHICS_EVENT_SEND_FAILURE){
        s.last_send_status=event.result;
    }else if(event.kind==LEO_GRAPHICS_EVENT_STALE_SEND){leo_graphics_inc(&s.stale_completions);
    }else if(event.kind==LEO_GRAPHICS_EVENT_COPY_SEND_FAILURE){leo_graphics_inc(&s.copy_send_failures);}
    if(published){
        if(s.events_published<LEO_GRAPHICS_EVENT_CAPACITY){
            LeoGraphicsEvent copy=event;copy.sequence=s.events_published+1U;
            leo_graphics_events[s.events_published++]=copy;
        }else leo_graphics_drop();
    }
    leo_graphics_unlock();
}
static void leo_graphics_send_attempt(void)
{
    if(!leo_graphics_try())return;
    leo_graphics_inc(&leo_graphics_state.send_calls);leo_graphics_unlock();
}
static void leo_graphics_send_result(uint32_t result)
{
    if(!leo_graphics_try())return;
    leo_graphics_state.last_send_status=result;
    if(!result)leo_graphics_inc(&leo_graphics_state.send_success);
    else leo_graphics_inc(&leo_graphics_state.send_failures);
    leo_graphics_unlock();
}
extern "C" uint32_t ios7leo_graphics_snapshot(LeoGraphicsState *out)
{
    if(!out||!leo_graphics_try())return 0;
    *out=leo_graphics_state;out->version=LEO_GRAPHICS_STATE_VERSION;out->known=1;
    out->loss=leo_graphics_losses;out->unknown=leo_graphics_unknown;leo_graphics_unlock();return 1;
}
extern "C" uint32_t ios7leo_graphics_event_take(LeoGraphicsEvent *out,uint32_t *lost)
{
    if(!out||!lost||!leo_graphics_try())return 0;
    *lost=leo_graphics_losses;
    if(leo_graphics_state.events_taken==leo_graphics_state.events_published){leo_graphics_unlock();return 0;}
    *out=leo_graphics_events[leo_graphics_state.events_taken++];leo_graphics_unlock();return 1;
}

#if defined(BOARD_CONFIG_QSD8250_LEO)
static IOLock *primaryLock;
static IOMemoryDescriptor *primaryPixels;
static IOBufferMemoryDescriptor *primaryControl;
static uint32_t primaryBytes;
static IOMemoryMap *primaryKernelPixels;
static uint64_t scanoutSequence;
static uint32_t displayBudget;
static const uint32_t leoWidth=480,leoHeight=800,leoRow=1920;
static const uint32_t primarySurfaceID = 1;
static uint32_t activeScanoutSurfaceID = primarySurfaceID;
enum { IOS7LAB_MAX_OFFSCREEN = 64 };

struct IOS7LabOffscreenSurface {
    uint32_t id;
    uint32_t references;
    uint32_t width, height, rowBytes, allocationSize, pixelFormat, scanoutPhysical,chargedBytes;
    bool preparedUserBuffer;
    IOMemoryDescriptor *pixels;
    IOMemoryDescriptor *control;
    IOMemoryMap *kernelPixels;
    IOMemoryMap *kernelControl;
};

static IOS7LabOffscreenSurface offscreenSurfaces[IOS7LAB_MAX_OFFSCREEN];
static uint32_t nextOffscreenSurfaceID = 1;
static uint32_t offscreenHeapBytes;
/* Ordinary worker only; observes the actual default CPU surface, never an
 * arbitrary client/offscreen address. The TRYLOCK retains backing identity. */
extern "C" void ios7leo_primary_observe(LeoDisplayObservation *out)
{
 if(!out || out->version!=1 || !primaryLock)return;
 if(!IOLockTryLock(primaryLock)){out->known|=LEO_OBS_PRIMARY_LOCK_BUSY;return;}
 if(primaryKernelPixels && primaryPixels && primaryControl && primaryBytes==1536000U &&
    primaryKernelPixels->getAddress() && primaryKernelPixels->getLength()>=primaryBytes &&
    primaryControl->getBytesNoCopy()){
  volatile const uint32_t *pixels=(volatile const uint32_t *)primaryKernelPixels->getAddress();
  volatile const uint32_t *control=(volatile const uint32_t *)primaryControl->getBytesNoCopy();
  out->primary_bytes=primaryBytes;out->primary_seed_before=control[2];out->active_surface=activeScanoutSurfaceID;
  uint32_t hash=2166136261U,rgb=0,alpha=0;
  for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
   unsigned index=((y*799U)/31U)*480U+(x*479U)/31U;
   uint32_t value=pixels[index];hash=(hash^value)*16777619U;
   if(value&0x00ffffffU)++rgb;if(value&0xff000000U)++alpha;
  }
  out->primary_seed_after=control[2];out->primary_sample_hash=hash;
  out->primary_nonzero_rgb=rgb;out->primary_nonzero_alpha=alpha;out->primary_samples=1024;
  out->known|=LEO_OBS_PRIMARY_SAMPLES;
 }
 IOLockUnlock(primaryLock);
}
static const uint32_t offscreenPixelBytes = 1536000U;
static bool ios7leo_memory_room(uint32_t additional)
{
 uint64_t managed=sane_size>>12,freePages=vm_page_free_count;
 uint64_t guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 uint64_t pages=((uint64_t)primaryBytes+4096U+offscreenHeapBytes+additional+65536U+4095U)>>12;
 return managed && pages<=(managed>>3) && freePages>=guard &&
  ((uint64_t)additional+4095U)/4096U<=(freePages-guard)/2U;
}

static const char ios7lab_fullscreen_surface_request[] =
    "<dict><key>IOSurfaceWidth</key><integer size=\"32\">0x1e0</integer>"
    "<key>IOSurfaceMemoryRegion</key><string>PurpleGfxMem</string>"
    "<key>IOSurfaceIsGlobal</key><true/>"
    "<key>IOSurfaceBytesPerRow</key><integer size=\"32\">0x780</integer>"
    "<key>IOSurfacePixelFormat</key><integer size=\"32\">0x42475241</integer>"
    "<key>IOSurfaceBytesPerElement</key><integer size=\"32\">0x4</integer>"
    "<key>IOSurfaceCacheMode</key><integer size=\"32\">0x400</integer>"
    "<key>IOSurfaceAllocSize</key><integer size=\"32\">0x177000</integer>"
    "<key>IOSurfaceHeight</key><integer size=\"32\">0x320</integer></dict>";

enum {
    IOS7LabSwapMetaTransaction = 3, IOS7LabSwapMetaSurfaceZero = 4,
    IOS7LabSwapMetaMask = 5, IOS7LabSwapMetaLayout = 6,
    IOS7LabSwapMetaPrimaryReady = 7, IOS7LabSwapMetaBackend = 8,
    IOS7LabSwapMetaCompleted = 9
};

static bool ios7lab_fullframe_scanout_layout(const uint32_t *words, uint32_t surfaceID,
    uint32_t *metadataReason, uint32_t *metadataMismatch)
{
    /* Match the original single full-frame layout. The caller separately
     * resolves the ID to owned backing; transforms/crops remain unsupported. */
    if (!surfaceID) { *metadataReason = IOS7LabSwapMetaSurfaceZero; return false; }
    /* Original SwapEnd uses the initialization mask once, then the observed
     * steady-state mask on unchanged full-frame primary geometry. */
    if (!((words[67] == 0x80000007U && words[68] == 0x80000007U) ||
          (words[67] == 1U && words[68] == 1U))) {
        *metadataReason = IOS7LabSwapMetaMask;
        return false;
    }
    uint32_t expected[0x138 / 4];
    bzero(expected, sizeof(expected));
    memcpy(expected, words, 6 * sizeof(uint32_t)); /* Three original UInt64 timestamp pairs, not a synthetic physical-vblank event. */
    expected[6] = words[6];                       /* Separately validated transaction. */
    expected[7] = surfaceID;
    expected[13] = expected[29] = expected[53] = (uint32_t)PE_state.video.v_width;
    expected[14] = expected[30] = expected[54] = (uint32_t)PE_state.video.v_height;
    expected[43] = 1;
    expected[67] = words[67];
    expected[68] = words[68];
    expected[69] = 0xff000000U;
    bool originalResult = memcmp(expected, words, sizeof(expected)) == 0;
    if (!originalResult) {
        *metadataReason = IOS7LabSwapMetaLayout;
        /* Index only, no new payload dump; compare the SAME existing operands. */
        for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
            if (expected[i] != words[i]) { *metadataMismatch = i; break; }
    }
    return originalResult;
}

static bool ios7lab_primary_ready(void)
{
    if (!primaryLock) return false;
    IOLockLock(primaryLock);
    if (primaryPixels && primaryControl) {
        IOLockUnlock(primaryLock);
        return true;
    }
    if (!ios7leo_display_ready() || !ios7leo_memory_room(offscreenPixelBytes+8192U)) {
        IOLockUnlock(primaryLock);return false;
    }
    IOBufferMemoryDescriptor *pixels = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryKernelUserShared, offscreenPixelBytes,4096);
    IOBufferMemoryDescriptor *control = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryKernelUserShared,4096,4096);
    IOMemoryMap *kernel = pixels ? pixels->map(kIOMapCopybackCache) : 0;
    if (!pixels || !control || !kernel || !kernel->getAddress() || !control->getBytesNoCopy() ||
        (uint64_t)vm_page_free_count < (sane_size>>14)+(uint64_t)vm_page_free_reserved+vm_page_free_target) {
        if(kernel)kernel->release();if(pixels)pixels->release();if(control)control->release();
        IOLockUnlock(primaryLock);return false;
    }
    bzero((void *)kernel->getAddress(),offscreenPixelBytes);
    primaryKernelPixels=kernel;

    bzero(control->getBytesNoCopy(), 4096);
    ((uint32_t *)control->getBytesNoCopy())[2] = 1; /* Initial content generation. */
    primaryPixels = pixels;
    primaryControl = control;
    primaryBytes = offscreenPixelBytes;
    IOLog("Leo primary BGRA CPU surface id=1 %ux%u row=%u bytes=%u; physical outputRGB565 separate\n",leoWidth,leoHeight,leoRow,primaryBytes);
    IOLockUnlock(primaryLock);
    return true;
}

static IOReturn ios7lab_present_owned_scanout(uint32_t id)
{
    IOReturn result=kIOReturnNotFound;
    IOS7LabOffscreenSurface retired;bzero(&retired,sizeof(retired));
    IOLockLock(primaryLock);
    const uint8_t *bytes=0;uint32_t length=0,row=0,format=0;
    if(id==primarySurfaceID && primaryKernelPixels){bytes=(const uint8_t *)primaryKernelPixels->getAddress();length=primaryBytes;row=leoRow;format=0x42475241U;}
    else for(unsigned i=0;i<IOS7LAB_MAX_OFFSCREEN;i++){
        IOS7LabOffscreenSurface *v=&offscreenSurfaces[i];
        if(v->id==id && (v->references||id==activeScanoutSurfaceID) && v->kernelPixels && v->width==leoWidth && v->height==leoHeight){
            bytes=(const uint8_t *)v->kernelPixels->getAddress();length=v->allocationSize;row=v->rowBytes;format=v->pixelFormat;break;
        }
    }
    uint64_t completed=0;
    if(bytes)result=ios7leo_display_present(bytes,length,row,format,&completed)?kIOReturnSuccess:kIOReturnIOError;
    if(result==kIOReturnSuccess){
        uint32_t previous=activeScanoutSurfaceID;activeScanoutSurfaceID=id;scanoutSequence=completed;
        if(previous!=id)for(unsigned i=0;i<IOS7LAB_MAX_OFFSCREEN;i++){
            IOS7LabOffscreenSurface *v=&offscreenSurfaces[i];
            if(v->id==previous&&!v->references){retired=*v;offscreenHeapBytes-=v->chargedBytes;bzero(v,sizeof(*v));break;}
        }
    }
    IOLockUnlock(primaryLock);
    if(retired.kernelPixels)retired.kernelPixels->release();
    if(retired.kernelControl)retired.kernelControl->release();
    if(retired.pixels){if(retired.preparedUserBuffer)retired.pixels->complete(kIODirectionInOut);retired.pixels->release();}
    if(retired.control)retired.control->release();
    return result;
}
static IOReturn ios7lab_wait_owned_scanout(uint32_t id,unsigned maximumPolls=100)
{
    (void)maximumPolls;IOLockLock(primaryLock);
    /* Pixel copy is complete and retained frame lives in real scanout. A later
     * presentation need not retain the old source buffer to satisfy its fence. */
    IOReturn result=id && scanoutSequence && ios7leo_display_completed(scanoutSequence)?kIOReturnSuccess:kIOReturnNotReady;
    IOLockUnlock(primaryLock);return result;
}

class IOS7LabSurfaceClient : public IOUserClient {
    OSDeclareDefaultStructors(IOS7LabSurfaceClient);
    task_t ownerTask;
    IOLock *mappingLock;
    IOMemoryMap *pixelsMap;
    IOMemoryMap *controlMap;
    struct ClientOffscreenMap {
        uint32_t id;
        IOMemoryMap *pixels;
        IOMemoryMap *control;
    } offscreenMaps[IOS7LAB_MAX_OFFSCREEN];
    IOReturn describe(IOExternalMethodArguments *args);
    IOReturn describeOffscreen(uint32_t id, IOExternalMethodArguments *args, bool acquireReference);
    IOReturn createOffscreen(IOExternalMethodArguments *args);
    IOReturn createSimple(IOExternalMethodArguments *args);
    IOReturn createSurface(const IOS7LabSimpleSurfaceRequest *geometry, IOExternalMethodArguments *args);
    void releaseOffscreen(unsigned index);
    void releaseMaps(void);
public:
    virtual IOService *getService(void) { return getProvider(); }
    virtual bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties);
    virtual void free(void);
    virtual IOReturn clientClose(void);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch = 0, OSObject *target = 0, void *reference = 0);
};

class IOSurfaceRoot : public IOService {
    OSDeclareDefaultStructors(IOSurfaceRoot);
public:
    virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
        OSDictionary *properties, IOUserClient **handler);
};

/* Original iOS7 user client discovery still uses this earlier class name. */
class IOCoreSurfaceRoot : public IOSurfaceRoot {
    OSDeclareDefaultStructors(IOCoreSurfaceRoot);
};

OSDefineMetaClassAndStructors(IOS7LabSurfaceClient, IOUserClient);
OSDefineMetaClassAndStructors(IOSurfaceRoot, IOService);
OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOSurfaceRoot);

bool IOS7LabSurfaceClient::initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties)
{
    ownerTask = 0;
    mappingLock = 0;
    pixelsMap = controlMap = 0;
    bzero(offscreenMaps, sizeof(offscreenMaps));
    if (!task || !IOUserClient::initWithTask(task, security, type, properties)) return false;
    mappingLock = IOLockAlloc();
    if (!mappingLock) return false;
    task_reference(task);
    ownerTask = task;
    return true;
}

void IOS7LabSurfaceClient::releaseMaps(void)
{
    if (pixelsMap) { pixelsMap->release(); pixelsMap = 0; }
    if (controlMap) { controlMap->release(); controlMap = 0; }
}

void IOS7LabSurfaceClient::releaseOffscreen(unsigned index)
{
    if (index >= IOS7LAB_MAX_OFFSCREEN || !offscreenMaps[index].id) return;
    uint32_t id = offscreenMaps[index].id;
    if (offscreenMaps[index].pixels) offscreenMaps[index].pixels->release();
    if (offscreenMaps[index].control) offscreenMaps[index].control->release();
    bzero(&offscreenMaps[index], sizeof(offscreenMaps[index]));

    IOMemoryDescriptor *pixels = 0;
    IOMemoryDescriptor *control = 0;
    IOMemoryMap *kernelPixels = 0;
    IOMemoryMap *kernelControl = 0;
    bool preparedUserBuffer = false;
    IOLockLock(primaryLock);
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
        IOS7LabOffscreenSurface *surface = &offscreenSurfaces[i];
        if (surface->id != id) continue;
        if (surface->references) --surface->references;
        if (!surface->references && id != activeScanoutSurfaceID) {
            pixels = surface->pixels;
            control = surface->control;
            kernelPixels = surface->kernelPixels;
            kernelControl = surface->kernelControl;
            preparedUserBuffer = surface->preparedUserBuffer;
            if (!surface->scanoutPhysical) offscreenHeapBytes -= surface->chargedBytes;
            bzero(surface, sizeof(*surface));
        }
        break;
    }
    IOLockUnlock(primaryLock);
    if (kernelPixels) kernelPixels->release();
    if (kernelControl) kernelControl->release();
    if (pixels) {
        if (preparedUserBuffer) pixels->complete(kIODirectionInOut);
        pixels->release();
    }
    if (control) control->release();
}

IOReturn IOS7LabSurfaceClient::describeOffscreen(uint32_t id,
    IOExternalMethodArguments *args, bool acquireReference)
{
    if (!id || !args->structureOutput || args->structureOutputSize < 0x548 ||
        args->scalarOutputCount) return kIOReturnBadArgument;

    int mapIndex = -1;
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
        if (offscreenMaps[i].id == id) { mapIndex = (int)i; acquireReference = false; break; }
        if (mapIndex < 0 && !offscreenMaps[i].id) mapIndex = (int)i;
    }
    if (mapIndex < 0) return kIOReturnNoMemory;

    IOMemoryDescriptor *pixels = 0;
    IOMemoryDescriptor *control = 0;
    IOMemoryMap *kernelControl = 0;
    IOS7LabSimpleSurfaceRequest geometry;
    bzero(&geometry, sizeof(geometry));
    IOLockLock(primaryLock);
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
        if (offscreenSurfaces[i].id != id) continue;
        pixels = offscreenSurfaces[i].pixels;
        control = offscreenSurfaces[i].control;
        kernelControl = offscreenSurfaces[i].kernelControl;
        geometry.width = offscreenSurfaces[i].width;
        geometry.height = offscreenSurfaces[i].height;
        geometry.bytesPerRow = offscreenSurfaces[i].rowBytes;
        geometry.allocationSize = offscreenSurfaces[i].allocationSize;
        geometry.pixelFormat = offscreenSurfaces[i].pixelFormat;
        if (acquireReference) ++offscreenSurfaces[i].references;
        break;
    }
    IOLockUnlock(primaryLock);
    if (!pixels || !control || !kernelControl || !kernelControl->getAddress())
        return kIOReturnNotFound;

    ClientOffscreenMap *mapping = &offscreenMaps[mapIndex];
    if (!mapping->id) mapping->id = id;
    if (!mapping->pixels)
        mapping->pixels = pixels->createMappingInTask(ownerTask, 0, kIOMapAnywhere | kIOMapCopybackCache);
    if (!mapping->control)
        mapping->control = control->createMappingInTask(ownerTask, 0, kIOMapAnywhere | kIOMapCopybackCache);
    if (!mapping->pixels || !mapping->control || !mapping->pixels->getAddress() ||
        !mapping->control->getAddress() ||
        (uint64_t)mapping->pixels->getAddress() + geometry.allocationSize > 0x40000000ULL ||
        (uint64_t)mapping->control->getAddress() + 4096 > 0x40000000ULL) {
        releaseOffscreen((unsigned)mapIndex);
        return kIOReturnNoMemory;
    }

    uint32_t info[0x548 / 4];
    bzero(info, sizeof(info));
    info[0] = (uint32_t)mapping->pixels->getAddress();
    info[2] = (uint32_t)mapping->control->getAddress();
    info[4] = id;
    info[5] = geometry.allocationSize;
    info[6] = geometry.width;
    info[7] = geometry.height;
    info[8] = geometry.bytesPerRow;
    info[10] = geometry.pixelFormat;
    info[14] = 0x01010004U; /* uint16 bytes=4, uint8 elementWidth=1, uint8 elementHeight=1 */
    info[15] = 1;
    info[16] = ((uint32_t *)kernelControl->getAddress())[2];
    memcpy(args->structureOutput, info, sizeof(info));
    args->structureOutputSize = sizeof(info);
    static unsigned int mapLogs;
    if (mapLogs++ < 32)
        IOLog("IOS7LAB offscreen surface id=%u mapped-pixels=%08x mapped-control=%08x bytes=%u\n",
              id, info[0], info[2], info[5]);
    return kIOReturnSuccess;
}

IOReturn IOS7LabSurfaceClient::createOffscreen(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount || !args->structureInput ||
        args->structureInputSize != sizeof(ios7lab_fullscreen_surface_request) ||
        memcmp(args->structureInput, ios7lab_fullscreen_surface_request,
               sizeof(ios7lab_fullscreen_surface_request)) != 0 ||
        !args->structureOutput || args->structureOutputSize < 0x548 ||
        args->scalarOutputCount) return kIOReturnUnsupported;

    IOS7LabSimpleSurfaceRequest geometry = {0, 0, 480, 800, 0x42475241U, 4, 1920, offscreenPixelBytes};
    return createSurface(&geometry, args);
}

IOReturn IOS7LabSurfaceClient::createSimple(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount || !args->structureInput ||
        args->structureInputSize != sizeof(IOS7LabSimpleSurfaceRequest) ||
        !args->structureOutput || args->structureOutputSize < 0x548 ||
        args->scalarOutputCount) return kIOReturnBadArgument;
    IOS7LabSimpleSurfaceRequest input, geometry;
    memcpy(&input, args->structureInput, sizeof(input));
    static unsigned logs;
    if (logs++ < 8)
        IOLog("IOS7LAB simple surface request address=%08x:%08x width=%u height=%u format=%x element=%u row=%u alloc=%u\n",
              input.memoryAddressHigh, input.memoryAddressLow, input.width, input.height, input.pixelFormat,
              input.bytesPerElement, input.bytesPerRow, input.allocationSize);
    if (!ios7lab_surface_geometry(&input, &geometry)) return kIOReturnUnsupported;
    return createSurface(&geometry, args);
}

IOReturn IOS7LabSurfaceClient::createSurface(const IOS7LabSimpleSurfaceRequest *geometry,
                                           IOExternalMethodArguments *args)
{
    uint32_t id = 0;
    bool userBuffer = geometry->memoryAddressLow != 0;
    IOLockLock(primaryLock);
    uint32_t physical = 0;
    uint64_t covered=(uint64_t)(userBuffer?(geometry->memoryAddressLow&4095U):0U)+geometry->allocationSize;
    uint32_t charge=(uint32_t)((covered+4095U)&~4095ULL)+4096U;
    uint64_t total=(uint64_t)primaryBytes+4096U+offscreenHeapBytes+charge+65536U;
    if (total > displayBudget || !ios7leo_memory_room(charge)) {
        IOLockUnlock(primaryLock);return kIOReturnNoMemory;
    }
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
        if (offscreenSurfaces[i].id) continue;
        IOMemoryDescriptor *pixels = userBuffer ? IOMemoryDescriptor::withAddressRange(
            (mach_vm_address_t)geometry->memoryAddressLow, geometry->allocationSize,
            kIODirectionInOut, ownerTask) :
            IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared,
                                                 geometry->allocationSize, 4096);
        IOMemoryDescriptor *control = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, 4096, 4096);
        bool preparedUserBuffer = userBuffer && pixels && pixels->prepare(kIODirectionInOut) == kIOReturnSuccess;
        if (userBuffer && !preparedUserBuffer) {
            if (pixels) pixels->release();
            if (control) control->release();
            break;
        }
        IOMemoryMap *kernelPixels = pixels ? pixels->map(kIOMapCopybackCache) : 0;
        IOMemoryMap *kernelControl = control ? control->map(kIOMapCopybackCache) : 0;
        if (!pixels || !control || !kernelPixels || !kernelControl ||
            !kernelPixels->getAddress() || !kernelControl->getAddress() ||
            (uint64_t)vm_page_free_count < (sane_size>>14)+(uint64_t)vm_page_free_reserved+vm_page_free_target) {
            if (kernelPixels) kernelPixels->release();
            if (kernelControl) kernelControl->release();
            if (pixels) {
                if (preparedUserBuffer) pixels->complete(kIODirectionInOut);
                pixels->release();
            }
            if (control) control->release();
            break;
        }
        if (!userBuffer) bzero((void *)kernelPixels->getAddress(), geometry->allocationSize);
        bzero((void *)kernelControl->getAddress(), 4096);
        ((uint32_t *)kernelControl->getAddress())[2] = 1;
        do { ++nextOffscreenSurfaceID; } while (nextOffscreenSurfaceID <= primarySurfaceID);
        id = nextOffscreenSurfaceID;
        offscreenSurfaces[i].id = id;
        offscreenSurfaces[i].references = 1;
        offscreenSurfaces[i].width = geometry->width;
        offscreenSurfaces[i].height = geometry->height;
        offscreenSurfaces[i].rowBytes = geometry->bytesPerRow;
        offscreenSurfaces[i].allocationSize = geometry->allocationSize;
        offscreenSurfaces[i].chargedBytes = charge;
        offscreenSurfaces[i].pixelFormat = geometry->pixelFormat;
        offscreenSurfaces[i].scanoutPhysical = physical;
        offscreenSurfaces[i].preparedUserBuffer = preparedUserBuffer;
        offscreenSurfaces[i].pixels = pixels;
        offscreenSurfaces[i].control = control;
        offscreenSurfaces[i].kernelPixels = kernelPixels;
        offscreenSurfaces[i].kernelControl = kernelControl;
        offscreenHeapBytes += charge;
        if (userBuffer)
            IOLog("IOS7LAB surface wraps client image id=%u width=%u height=%u row=%u bytes=%u\n",
                  id, geometry->width, geometry->height, geometry->bytesPerRow, geometry->allocationSize);
        break;
    }
    IOLockUnlock(primaryLock);
    if (!id) {
        return kIOReturnNoResources;
    }
    IOReturn result = describeOffscreen(id, args, false);
    if (result != kIOReturnSuccess) {
        for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i)
            if (offscreenMaps[i].id == id) { releaseOffscreen(i); break; }
    }
    return result;
}

void IOS7LabSurfaceClient::free(void)
{
    releaseMaps();
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i)
        releaseOffscreen(i);
    if (mappingLock) { IOLockFree(mappingLock); mappingLock = 0; }
    if (ownerTask) { task_deallocate(ownerTask); ownerTask = 0; }
    IOUserClient::free();
}

IOReturn IOS7LabSurfaceClient::clientClose(void)
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn IOS7LabSurfaceClient::describe(IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < 0x548 || args->scalarOutputCount)
        return kIOReturnBadArgument;
    if (!ios7lab_primary_ready()) return kIOReturnNoMemory;
    if (!pixelsMap) pixelsMap = primaryPixels->createMappingInTask(ownerTask, 0, kIOMapAnywhere | kIOMapCopybackCache);
    if (!controlMap) controlMap = primaryControl->createMappingInTask(ownerTask, 0, kIOMapAnywhere | kIOMapCopybackCache);
    if (!pixelsMap || !controlMap || !pixelsMap->getAddress() || !controlMap->getAddress() ||
        (uint64_t)pixelsMap->getAddress() + primaryBytes > 0x40000000ULL ||
        (uint64_t)controlMap->getAddress() + 4096 > 0x40000000ULL) {
        releaseMaps();
        IOLog("IOS7LAB primary surface could not map into client task\n");
        return kIOReturnNoMemory;
    }
    /* Original client object has an 8-byte local prefix before this wire data.
     * Slots are verified by original GetBaseAddress/ID/AllocSize/geometry/seed
     * getters. One linear BGRA plane uses planeCount0, no optional planes.
     */
    uint32_t info[0x548 / 4];
    bzero(info, sizeof(info));
    info[0] = (uint32_t)pixelsMap->getAddress();
    /* Unobserved companion/high pointer words stay zero for ARM32 maps. */
    info[2] = (uint32_t)controlMap->getAddress();
    info[4] = primarySurfaceID;
    info[5] = primaryBytes;
    info[6] = (uint32_t)PE_state.video.v_width;
    info[7] = (uint32_t)PE_state.video.v_height;
    info[8] = leoRow;
    info[10] = 0x42475241U; /* BGRA */
    info[14] = 0x01010004U; /* uint16 bytes=4, uint8 elementWidth=1, uint8 elementHeight=1 */
    info[15] = 1;           /* Preserve existing adjacent descriptor word; not elementHeight. */
    info[16] = ((uint32_t *)primaryControl->getBytesNoCopy())[2];
    memcpy(args->structureOutput, info, sizeof(info));
    args->structureOutputSize = sizeof(info);
    static unsigned int map_logs;
    if (map_logs++ < 32)
        IOLog("IOS7LAB surface lookup id=1 mapped-pixels=%08x mapped-control=%08x bytes=%u\n", info[0], info[2], primaryBytes);
    return kIOReturnSuccess;
}

IOReturn IOS7LabSurfaceClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    static unsigned int logs;
    if (!args) return kIOReturnBadArgument;
    if (logs++ < 96)
        IOLog("IOS7LAB surface method=%u scalar-in=%u struct-in=%u scalar-out=%u struct-out=%u\n",
              selector, args->scalarInputCount, args->structureInputSize, args->scalarOutputCount, args->structureOutputSize);
    if (args->structureInputDescriptor || args->structureOutputDescriptor) return kIOReturnUnsupported;
    IOReturn result = kIOReturnUnsupported;
    IOLockLock(mappingLock);
    if (selector == 0) {
        result = createOffscreen(args);
    } else if (selector == 8) {
        result = createSimple(args);
    } else if (selector == 16 && !args->scalarInputCount && !args->structureInputSize &&
               !args->scalarOutputCount && args->structureOutput &&
               args->structureOutputSize >= sizeof(IOS7LabSurfaceCapabilities)) {
        IOS7LabSurfaceCapabilities capabilities = ios7lab_surface_capabilities();
        memcpy(args->structureOutput, &capabilities, sizeof(capabilities));
        args->structureOutputSize = sizeof(capabilities);
        result = kIOReturnSuccess;
        static unsigned int capabilityLogs;
        if (capabilityLogs++ < 4)
            IOLog("IOS7LAB native surface capabilities row-align=%u offset-align=%u max-row=%u max-width=%u max-height=%u\n",
                capabilities.rowAlignmentMask + 1, capabilities.offsetAlignmentMask + 1,
                capabilities.maximumRowBytes, capabilities.maximumWidth, capabilities.maximumHeight);
    } else if (selector == 25 && args->scalarInputCount == 1 && args->scalarInput &&
               !args->structureInputSize && args->scalarOutputCount == 1 &&
               args->scalarOutput && !args->structureOutputSize) {
        /* Native IOSurfaceClientIsTiled; every mapped CPU surface is linear. */
        uint64_t id = args->scalarInput[0];
        bool owned = id == primarySurfaceID && pixelsMap;
        for (unsigned i = 0; !owned && i < IOS7LAB_MAX_OFFSCREEN; ++i)
            owned = offscreenMaps[i].id == id && offscreenMaps[i].pixels;
        result = owned ? kIOReturnSuccess : kIOReturnNotFound;
        if (owned) args->scalarOutput[0] = 0;
    } else if ((selector == 6 || selector == 1) && args->scalarInputCount == 1 &&
               args->scalarInput && !args->structureInputSize) {
        uint32_t id = (uint32_t)args->scalarInput[0];
        if (selector == 6) {
            result = id == primarySurfaceID ? describe(args) : describeOffscreen(id, args, true);
        } else if (!args->scalarOutputCount && !args->structureOutputSize) {
            if (id == primarySurfaceID) { releaseMaps(); result = kIOReturnSuccess; }
            else {
                result = kIOReturnNotFound;
                for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
                    if (offscreenMaps[i].id != id) continue;
                    releaseOffscreen(i);
                    result = kIOReturnSuccess;
                    break;
                }
            }
        }
    } else if ((selector == 2 || selector == 3) && !args->scalarInputCount &&
               args->structureInput && args->structureInputSize == 12 && !args->scalarOutputCount) {
        uint32_t request[3];
        memcpy(request, args->structureInput, sizeof(request));
        if (request[1] & ~3U) result = kIOReturnUnsupported;
        else if (selector == 2) {
            __sync_synchronize(); /* CPU-only backing has no outstanding GPU work. */
            result = request[0] == primarySurfaceID ? describe(args) :
                describeOffscreen(request[0], args, false);
        } else if (args->structureOutput && args->structureOutputSize >= 4) {
            __sync_synchronize();
            IOLockLock(primaryLock);
            void *controlBytes = 0;
            if (request[0] == primarySurfaceID && primaryControl)
                controlBytes = primaryControl->getBytesNoCopy();
            else for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i)
                if (offscreenSurfaces[i].id == request[0] && offscreenSurfaces[i].kernelControl) {
                    controlBytes = (void *)offscreenSurfaces[i].kernelControl->getAddress();
                    break;
                }
            if (controlBytes) {
                uint32_t *state = (uint32_t *)controlBytes;
                if (!(request[1] & 1U)) ++state[2];
                memcpy(args->structureOutput, &state[2], 4);
                args->structureOutputSize = 4;
                result = kIOReturnSuccess;
            } else result = kIOReturnNotFound;
            IOLockUnlock(primaryLock);
        }
    }
    IOLockUnlock(mappingLock);
    /* Report actual unsupported native calls even after routine traffic has
     * exhausted its log budget. These are ordinary driver errors, bounded. */
    static unsigned int failures[32];
    if (result != kIOReturnSuccess && selector < 32 && failures[selector]++ < 4)
        IOLog("IOS7LAB surface failure selector=%u result=%x scalar-in=%u struct-in=%u scalar-out=%u struct-out=%u\n",
            selector, result, args->scalarInputCount, args->structureInputSize,
            args->scalarOutputCount, args->structureOutputSize);
    return result;
}

IOReturn IOSurfaceRoot::newUserClient(task_t task, void *security, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
    IOLog("IOS7LAB surface open type=%u\n", (unsigned int)type);
    if (type != 0 || !handler) return kIOReturnBadArgument;
    *handler = 0;
    IOS7LabSurfaceClient *client = new IOS7LabSurfaceClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) { client->release(); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    *handler = client;
    return kIOReturnSuccess;
}

class IOS7LabFramebufferClient : public IOUserClient {
    OSDeclareDefaultStructors(IOS7LabFramebufferClient);
    IOLock *transactionLock;
    uint32_t nextTransaction;
    uint32_t activeTransaction;
    /* Private diagnostic state only: no exported/wire/vtable ABI change. */
    struct SwapMetadataState {
        uint64_t sampleInterval, lastSample, lastCompleted;
        uint32_t preCount, postCount;
        bool physicalSeen;
    } swapMetadata;
    struct WaitMetadataState {
        uint64_t lastSample;
        uint32_t preCount, postCount;
    } waitMetadata;
    void noteWaitMetadata(uint32_t reason, uint32_t known,
        uint64_t transaction, uint64_t options, uint64_t timeout,
        uint32_t observedTransaction, uint32_t observedSurface, bool observedComplete,
        uint64_t begin, uint64_t backendBegin, uint64_t end, IOReturn result);
    void noteSwapMetadata(uint32_t reason, uint32_t known, uint32_t mismatch, const uint32_t *words,
        const IOExternalMethodArguments *args, uint64_t begin, uint64_t backendBegin,
        uint64_t end, IOReturn result);
    struct ScanoutCompletion {
        uint32_t transaction, surface;
        bool completed;
    } completions[32];
    IOWorkLoop *vsyncWorkLoop;
    IOTimerEventSource *vsyncTimer;
    mach_port_t vsyncPort;
    io_user_reference_t vsyncCallback;
    io_user_reference_t vsyncRefcon;
    bool vsyncEnabled;
    bool vsyncRequested,vsyncClosed;
    uint64_t vsyncCount;
    uint64_t vsyncGeneration;
    uint32_t graphicsClient,graphicsOwnerPID;
    LeoGraphicsEvent graphicsEvent(uint32_t kind,uint32_t selector,IOReturn result,
        const LeoGraphicsCall &note);
    void noteGraphics(uint32_t kind,uint32_t selector,IOReturn result,
        const LeoGraphicsCall &note,bool published=true);
    IOReturn applyVsyncTimer(bool enabled,uint64_t generation);
    static IOReturn vsyncWorkAction(OSObject *,void *,void *,void *,void *);
    IOReturn registerNotificationPortGated(mach_port_t,UInt32,io_user_reference_t);
    IOReturn setVsyncCallbackGated(io_user_reference_t,io_user_reference_t);
    void emitVsyncGated(IOTimerEventSource *);
    void stopVsyncGated(void);
    IOReturn performExternalMethod(uint32_t selector,IOExternalMethodArguments *args,
        LeoGraphicsCall *note);
    static void vsyncTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void emitVsync(IOTimerEventSource *sender);
    void stopVsync(void);
public:
    virtual IOService *getService(void) { return getProvider(); }
    virtual bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties);
    virtual void free(void);
    virtual IOReturn clientClose(void);
    virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
        io_user_reference_t refCon);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch = 0, OSObject *target = 0, void *reference = 0);
};

class IOMobileFramebuffer : public IOService {
    OSDeclareDefaultStructors(IOMobileFramebuffer);
public:
    virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
        OSDictionary *properties, IOUserClient **handler);
};

/* Original CreateDisplayList uses IOServiceMatching("AppleCLCD") first. */
class AppleCLCD : public IOMobileFramebuffer {
    OSDeclareDefaultStructors(AppleCLCD);
};

OSDefineMetaClassAndStructors(IOS7LabFramebufferClient, IOUserClient);
OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

bool IOS7LabFramebufferClient::initWithTask(task_t task, void *security,
    UInt32 type, OSDictionary *properties)
{
    transactionLock = 0;
    nextTransaction = activeTransaction = 0;
    bzero(&swapMetadata, sizeof(swapMetadata));
    bzero(&waitMetadata, sizeof(waitMetadata));
    AbsoluteTime metadataInterval;
    clock_interval_to_absolutetime_interval(5, NSEC_PER_SEC, &metadataInterval);
    swapMetadata.sampleInterval = __OSAbsoluteTime(metadataInterval);
    bzero(completions, sizeof(completions));
    vsyncWorkLoop = 0;
    vsyncTimer = 0;
    vsyncPort = MACH_PORT_NULL;
    vsyncCallback = vsyncRefcon = 0;
    vsyncEnabled = false;
    vsyncRequested=vsyncClosed=false;
    vsyncCount = 0;
    vsyncGeneration=1;graphicsClient=graphicsOwnerPID=0;
    if (!IOUserClient::initWithTask(task, security, type, properties))
        return false;
    transactionLock = IOLockAlloc();
    vsyncWorkLoop = IOWorkLoop::workLoop();
    vsyncTimer = IOTimerEventSource::timerEventSource(this,
        &IOS7LabFramebufferClient::vsyncTimerFired);
    if (!transactionLock || !vsyncWorkLoop || !vsyncTimer ||
        vsyncWorkLoop->addEventSource(vsyncTimer) != kIOReturnSuccess)
        return false;
    graphicsClient=(uint32_t)OSIncrementAtomic((volatile SInt32 *)&leo_graphics_client_ids)+1U;
    void *process=task?get_bsdtask_info(task):0;
    int pid=process?proc_pid((struct proc *)process):0;
    graphicsOwnerPID=pid>0?(uint32_t)pid:0;
    leo_graphics_open(graphicsClient,graphicsOwnerPID);
    return true;
}

/* Caller holds transactionLock. Flat values only; no port or user dereference. */
LeoGraphicsEvent IOS7LabFramebufferClient::graphicsEvent(uint32_t kind,
    uint32_t selector,IOReturn result,const LeoGraphicsCall &note)
{
    LeoGraphicsEvent event;bzero(&event,sizeof(event));
    event.kind=kind;event.client=graphicsClient;event.owner_pid=graphicsOwnerPID;
    event.selector=selector;event.result=(uint32_t)result;event.reason=note.reason;
    event.transaction_lo=(uint32_t)note.transaction;event.transaction_hi=(uint32_t)(note.transaction>>32);
    event.surface=note.surface;event.mismatch=note.mismatch;
    event.generation_lo=(uint32_t)vsyncGeneration;event.generation_hi=(uint32_t)(vsyncGeneration>>32);
    event.requested=vsyncRequested;event.enabled=vsyncEnabled;event.closed=vsyncClosed;
    event.port_present=IPC_PORT_VALID(vsyncPort);event.callback_present=vsyncCallback!=0;
    event.send_count_lo=(uint32_t)vsyncCount;event.send_count_hi=(uint32_t)(vsyncCount>>32);
    return event;
}
void IOS7LabFramebufferClient::noteGraphics(uint32_t kind,uint32_t selector,
    IOReturn result,const LeoGraphicsCall &note,bool published)
{
    IOLockLock(transactionLock);LeoGraphicsEvent event=graphicsEvent(kind,selector,result,note);
    IOLockUnlock(transactionLock);leo_graphics_note(event,note.backend,published);
}
/* Workloop gate serializes config writers with timer callbacks. Timer APIs
 * never execute while transactionLock is held (the callback needs that lock). */
IOReturn IOS7LabFramebufferClient::applyVsyncTimer(bool enabled,uint64_t generation)
{
    IOReturn result=kIOReturnSuccess;
    if(enabled)result=vsyncTimer->setTimeoutUS(16667);
    else vsyncTimer->cancelTimeout();
    IOLockLock(transactionLock);
    bool current=vsyncGeneration==generation;
    if(current && result!=kIOReturnSuccess)vsyncEnabled=false;
    LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_TIMER,UINT32_MAX,false};
    LeoGraphicsEvent event=graphicsEvent(LEO_GRAPHICS_EVENT_TIMER_FAILURE,9,result,note);
    IOLockUnlock(transactionLock);
    if(current){
        if(leo_graphics_try()){leo_graphics_state.last_timer_status=(uint32_t)result;leo_graphics_unlock();}
        leo_graphics_note(event,false,result!=kIOReturnSuccess);
    }
    return result;
}

IOReturn IOS7LabFramebufferClient::vsyncWorkAction(OSObject *target,void *arg0,
    void *,void *,void *)
{
    if(!target || !arg0)return kIOReturnBadArgument;
    IOS7LabFramebufferClient *client=static_cast<IOS7LabFramebufferClient *>(target);
    LeoVsyncAction *action=static_cast<LeoVsyncAction *>(arg0);
    switch(action->operation){
        case LEO_VSYNC_PORT:return client->registerNotificationPortGated(action->port,action->type,action->refcon);
        case LEO_VSYNC_CALLBACK:return client->setVsyncCallbackGated(action->callback,action->refcon);
        case LEO_VSYNC_EMIT:client->emitVsyncGated(action->sender);return kIOReturnSuccess;
        case LEO_VSYNC_CLOSE:client->stopVsyncGated();return kIOReturnSuccess;
        default:return kIOReturnBadArgument;
    }
}

void IOS7LabFramebufferClient::stopVsync(void)
{
    if(!transactionLock)return;
    if(!vsyncWorkLoop){stopVsyncGated();return;} /* unpublished partial init */
    LeoVsyncAction action={LEO_VSYNC_CLOSE,MACH_PORT_NULL,0,0,0,0};
    (void)vsyncWorkLoop->runAction(&IOS7LabFramebufferClient::vsyncWorkAction,this,&action);
}

void IOS7LabFramebufferClient::stopVsyncGated(void)
{
    if (!transactionLock) return;
    IOLockLock(transactionLock);
    vsyncClosed=true;vsyncRequested=false;vsyncEnabled=false;
    if(!++vsyncGeneration)++vsyncGeneration;
    LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_CLOSED,UINT32_MAX,false};
    LeoGraphicsEvent event=graphicsEvent(LEO_GRAPHICS_EVENT_CLOSE,9,kIOReturnSuccess,note);
    IOLockUnlock(transactionLock);
    if(vsyncTimer)vsyncTimer->cancelTimeout();
    leo_graphics_note(event,false,true);
}

void IOS7LabFramebufferClient::vsyncTimerFired(OSObject *owner,
    IOTimerEventSource *sender)
{
    IOS7LabFramebufferClient *client = OSDynamicCast(IOS7LabFramebufferClient, owner);
    if (client) client->emitVsync(sender);
}

void IOS7LabFramebufferClient::emitVsync(IOTimerEventSource *sender)
{
    if(!sender || sender!=vsyncTimer || !vsyncWorkLoop || !transactionLock)return;
    LeoVsyncAction action={LEO_VSYNC_EMIT,MACH_PORT_NULL,0,0,0,sender};
    (void)vsyncWorkLoop->runAction(&IOS7LabFramebufferClient::vsyncWorkAction,this,&action);
}

void IOS7LabFramebufferClient::emitVsyncGated(IOTimerEventSource *sender)
{
    OSAsyncReference64 reference;
    uint64_t now = mach_absolute_time();
    AbsoluteTime intervalTime;
    clock_interval_to_absolutetime_interval(16667, NSEC_PER_USEC, &intervalTime);
    uint64_t interval = __OSAbsoluteTime(intervalTime);
    io_user_reference_t values[6] = {
        (uint32_t)now, (uint32_t)(now >> 32),
        (uint32_t)now, (uint32_t)(now >> 32),
        (uint32_t)interval, (uint32_t)(interval >> 32)
    };
    /* Public runAction recursively serializes this action with workloop timers.
     * IOTimerEventSource also invokes timer actions under this same gate.
     * Driver transactionLock is never held across IPC or timer APIs. */
    IOLockLock(transactionLock);
    if(vsyncClosed || !vsyncRequested || !vsyncEnabled ||
       !IPC_PORT_VALID(vsyncPort) || !vsyncCallback){
        IOLockUnlock(transactionLock);return;
    }
    mach_port_t port=vsyncPort;
    io_user_reference_t callback=vsyncCallback,refcon=vsyncRefcon;
    uint64_t generation=vsyncGeneration;
    IOLockUnlock(transactionLock);
    /* A real additional naked SEND right, not merely an object reference.
     * The workloop gate keeps the stored right stable until this copy exists. */
    mach_port_t heldPort=ipc_port_copy_send(port);
    if(!IPC_PORT_VALID(heldPort)){
        IOLockLock(transactionLock);
        bool current=vsyncGeneration==generation;
        if(current)vsyncEnabled=false;
        LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_PORT_COPY,UINT32_MAX,false};
        LeoGraphicsEvent event=graphicsEvent(LEO_GRAPHICS_EVENT_COPY_SEND_FAILURE,9,kIOReturnNotReady,note);
        IOLockUnlock(transactionLock);if(current)vsyncTimer->cancelTimeout();
        leo_graphics_note(event,false,true);return;
    }
    bzero(reference,sizeof(reference));
    setAsyncReference64(reference,heldPort,callback,refcon);
    IOLockLock(transactionLock);uint64_t emittedCount=++vsyncCount;
    IOLockUnlock(transactionLock);
    retain();sender->retain();
    leo_graphics_send_attempt();
    IOReturn result=sendAsyncResult64(reference,kIOReturnSuccess,values,6);
    releaseNotificationPort(heldPort);
    leo_graphics_send_result((uint32_t)result);
    static unsigned int logs;
    if(logs++<24)
        IOLog("Leo software-pacing notification count=%llu result=%x now=%llu interval=%llu\n",
              (unsigned long long)emittedCount,result,(unsigned long long)now,
              (unsigned long long)interval);
    if(result!=kIOReturnSuccess){
        static volatile unsigned int failureRecords;
        unsigned int slot=failureRecords;
        if(slot<8 && __sync_bool_compare_and_swap(&failureRecords,slot,slot+1))
            IOLog("IOS7LAB vsync-send failure record=%u count=%llu result=%x now=%llu interval=%llu\n",
                  slot+1,(unsigned long long)emittedCount,result,
                  (unsigned long long)now,(unsigned long long)interval);
    }
    IOLockLock(transactionLock);
    bool current=vsyncGeneration==generation;
    bool rearm=current && !vsyncClosed && vsyncRequested && vsyncEnabled &&
               IPC_PORT_VALID(vsyncPort) && vsyncCallback && result==kIOReturnSuccess;
    if(current && result!=kIOReturnSuccess)vsyncEnabled=false;
    LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_NONE,UINT32_MAX,false};
    LeoGraphicsEvent event=graphicsEvent(current?LEO_GRAPHICS_EVENT_SEND_FAILURE:
        LEO_GRAPHICS_EVENT_STALE_SEND,9,result,note);
    IOLockUnlock(transactionLock);
    if(current)(void)applyVsyncTimer(rearm,generation);
    if(!current || result!=kIOReturnSuccess)leo_graphics_note(event,false,true);
    sender->release();release();
}

IOReturn IOS7LabFramebufferClient::registerNotificationPort(mach_port_t port,
    UInt32 type,io_user_reference_t refCon)
{
    leo_graphics_port_entry(graphicsClient,graphicsOwnerPID,type,IPC_PORT_VALID(port));
    if(!transactionLock || !vsyncWorkLoop || !vsyncTimer){
        leo_graphics_port_preflight(kIOReturnNotReady);return kIOReturnNotReady;
    }
    LeoVsyncAction action={LEO_VSYNC_PORT,port,type,refCon,0,0};
    IOReturn result=vsyncWorkLoop->runAction(&IOS7LabFramebufferClient::vsyncWorkAction,this,&action);
    leo_graphics_port_return(result);return result;
}

IOReturn IOS7LabFramebufferClient::registerNotificationPortGated(mach_port_t port,
    UInt32 type, io_user_reference_t refCon)
{
    leo_graphics_port_gate_entry();
    LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_NONE,UINT32_MAX,false};
    if(type!=0){
        note.reason=LEO_GRAPHICS_REASON_ARGUMENT;
        noteGraphics(LEO_GRAPHICS_EVENT_PORT,UINT32_MAX,kIOReturnUnsupported,note);
        return kIOReturnUnsupported;
    }
    IOLockLock(transactionLock);
    if(vsyncClosed){
        note.reason=LEO_GRAPHICS_REASON_CLOSED;
        LeoGraphicsEvent event=graphicsEvent(LEO_GRAPHICS_EVENT_PORT,UINT32_MAX,kIOReturnNotOpen,note);
        IOLockUnlock(transactionLock);
        leo_graphics_note(event,false,true);return kIOReturnNotOpen;
    }
    mach_port_t oldPort=vsyncPort;
    vsyncPort=port;
    if(!++vsyncGeneration)++vsyncGeneration;
    uint64_t generation=vsyncGeneration;
    vsyncEnabled=vsyncRequested && IPC_PORT_VALID(vsyncPort) && vsyncCallback;
    bool enabled=vsyncEnabled;
    IOLockUnlock(transactionLock);
    IOReturn timerResult=applyVsyncTimer(enabled,generation);
    if(timerResult!=kIOReturnSuccess)note.reason=LEO_GRAPHICS_REASON_TIMER;
    noteGraphics(LEO_GRAPHICS_EVENT_PORT,UINT32_MAX,kIOReturnSuccess,note);
    if(IPC_PORT_VALID(oldPort))releaseNotificationPort(oldPort);
    IOLog("IOS7LAB framebuffer notification-port type=0 port=%x refcon=%llx\n",
          port, refCon);
    return kIOReturnSuccess;
}

IOReturn IOS7LabFramebufferClient::setVsyncCallbackGated(io_user_reference_t callback,
    io_user_reference_t refcon)
{
    IOLockLock(transactionLock);
    if(vsyncClosed){IOLockUnlock(transactionLock);return kIOReturnNotOpen;}
    vsyncCallback=callback;vsyncRefcon=refcon;vsyncRequested=callback!=0;
    if(!++vsyncGeneration)++vsyncGeneration;
    uint64_t generation=vsyncGeneration;
    vsyncEnabled=vsyncRequested && IPC_PORT_VALID(vsyncPort);
    bool enable=vsyncEnabled;
    IOLockUnlock(transactionLock);
    IOReturn result=applyVsyncTimer(enable,generation);
    IOLog("IOS7LAB framebuffer vsync %s callback=%llx refcon=%llx\n",
          enable && result==kIOReturnSuccess?"enabled":"disabled",callback,refcon);
    return result;
}

void IOS7LabFramebufferClient::free(void)
{
    stopVsync();
    if (vsyncWorkLoop && vsyncTimer) vsyncWorkLoop->removeEventSource(vsyncTimer);
    if (vsyncTimer) { vsyncTimer->release(); vsyncTimer = 0; }
    if (vsyncWorkLoop) { vsyncWorkLoop->release(); vsyncWorkLoop = 0; }
    if (vsyncPort) { releaseNotificationPort(vsyncPort); vsyncPort = MACH_PORT_NULL; }
    if (transactionLock) {
        IOLockFree(transactionLock);
        transactionLock = 0;
    }
    IOUserClient::free();
}

IOReturn IOS7LabFramebufferClient::clientClose(void)
{
    stopVsync();
    terminate();
    return kIOReturnSuccess;
}


void IOS7LabFramebufferClient::noteSwapMetadata(uint32_t reason, uint32_t known, uint32_t mismatch,
    const uint32_t *words, const IOExternalMethodArguments *args,
    uint64_t begin, uint64_t backendBegin, uint64_t end, IOReturn result)
{
    bool emit = false;
    uint32_t phase, sequence = 0;
    uint64_t previousCompletion;
    IOLockLock(transactionLock);
    previousCompletion = swapMetadata.lastCompleted;
    if (reason == IOS7LabSwapMetaCompleted && result == kIOReturnSuccess) {
        /* Called only AFTER real presentation and original ledger completion. */
        swapMetadata.physicalSeen = true;
        swapMetadata.lastCompleted = end;
    }
    phase = swapMetadata.physicalSeen ? 1U : 0U;
    if (!phase) {
        if (swapMetadata.preCount < 2) {
            sequence = ++swapMetadata.preCount;
            emit = true;
        }
    } else if (swapMetadata.postCount < 64 &&
               (swapMetadata.postCount < 4 ||
                (swapMetadata.sampleInterval && end >= swapMetadata.lastSample &&
                 end - swapMetadata.lastSample >= swapMetadata.sampleInterval))) {
        sequence = ++swapMetadata.postCount;
        swapMetadata.lastSample = end;
        emit = true;
    }
    IOLockUnlock(transactionLock);
    if (!emit) return;
    /* Reached only after exact wire validation and local copy. Early args,
     * descriptor and wire-shape returns remain byte-exact and uninstrumented. */
    IOLog("IOS7LAB swapmeta phase=%u seq=%u reason=%u known=%x result=%x header=%u,%u,%u,%u payload=%u transaction=%u surface=%u masks=%x,%x source=%ux%u middle=%ux%u output=%ux%u mismatch=%u begin=%llu backend-begin=%llu end=%llu previous-complete=%llu\n",
        phase, sequence, reason, known, result, args->scalarInputCount, args->structureInputSize,
        args->scalarOutputCount, args->structureOutputSize, words ? 1U : 0U,
        words ? words[6] : 0U, words ? words[7] : 0U,
        words ? words[67] : 0U, words ? words[68] : 0U,
        words ? words[13] : 0U, words ? words[14] : 0U,
        words ? words[29] : 0U, words ? words[30] : 0U,
        words ? words[53] : 0U, words ? words[54] : 0U, mismatch,
        (unsigned long long)begin, (unsigned long long)backendBegin, (unsigned long long)end,
        (unsigned long long)previousCompletion);
}

void IOS7LabFramebufferClient::noteWaitMetadata(uint32_t reason, uint32_t known,
    uint64_t transaction, uint64_t options, uint64_t timeout,
    uint32_t observedTransaction, uint32_t observedSurface, bool observedComplete,
    uint64_t begin, uint64_t backendBegin, uint64_t end, IOReturn result)
{
    bool emit = false;
    uint32_t phase, sequence = 0;
    IOLockLock(transactionLock);
    phase = swapMetadata.physicalSeen ? 1U : 0U;
    if (!phase) {
        if (waitMetadata.preCount < 2) { sequence = ++waitMetadata.preCount; emit = true; }
    } else if (waitMetadata.postCount < 64 &&
               (waitMetadata.postCount < 4 ||
                (swapMetadata.sampleInterval && end >= waitMetadata.lastSample &&
                 end - waitMetadata.lastSample >= swapMetadata.sampleInterval))) {
        sequence = ++waitMetadata.postCount;
        waitMetadata.lastSample = end;
        emit = true;
    }
    IOLockUnlock(transactionLock);
    if (!emit) return;
    IOLog("IOS7LAB waitmeta phase=%u seq=%u reason=%u known=%x result=%x requested=%llu options=%llx timeout=%llu observed=%u surface=%u completed=%u begin=%llu backend-begin=%llu end=%llu\n",
        phase, sequence, reason, known, result,
        (unsigned long long)transaction, (unsigned long long)options, (unsigned long long)timeout,
        observedTransaction, observedSurface, observedComplete ? 1U : 0U,
        (unsigned long long)begin, (unsigned long long)backendBegin, (unsigned long long)end);
}

IOReturn IOS7LabFramebufferClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *)
{
    bool watched=selector==4 || selector==5 || selector==6 || selector==9;
    bool prerequisite=selector==3 || selector==8 || selector==18;
    if(watched)leo_graphics_attempt(selector);
    if(prerequisite)leo_graphics_prereq_attempt(graphicsClient,graphicsOwnerPID,selector);
    LeoGraphicsCall note={0,0,LEO_GRAPHICS_REASON_ARGUMENT,UINT32_MAX,false};
    IOReturn result=performExternalMethod(selector,args,&note);
    if(prerequisite)leo_graphics_prereq_result(graphicsClient,graphicsOwnerPID,selector,result);
    if(watched)noteGraphics(LEO_GRAPHICS_EVENT_METHOD,selector,result,note);
    return result;
}

IOReturn IOS7LabFramebufferClient::performExternalMethod(uint32_t selector,
    IOExternalMethodArguments *args,LeoGraphicsCall *note)
{
    static unsigned int logs;
    if (!args)
        return kIOReturnBadArgument;
    if (logs++ < 128)
        IOLog("IOS7LAB framebuffer method=%u scalar-in=%u struct-in=%u scalar-out=%u struct-out=%u\n",
              selector, args->scalarInputCount, args->structureInputSize,
              args->scalarOutputCount, args->structureOutputSize);
    if (args->structureInputDescriptor || args->structureOutputDescriptor) {
        note->reason=LEO_GRAPHICS_REASON_DESCRIPTOR;return kIOReturnUnsupported;
    }
    if (selector == 5) {
        /* Original SwapEnd submits 0x138 bytes beginning at handle+0x10;
         * SwapBegin's ID at handle+0x28 therefore occupies offset0x18.
         * Accept only a full-size frame in a currently owned surface.
         */
        uint32_t words[0x138 / 4];
        static unsigned int captures;
        if (args->scalarInputCount || !args->structureInput ||
            args->structureInputSize != sizeof(words) ||
            args->scalarOutputCount || args->structureOutputSize)
            return kIOReturnBadArgument;
        memcpy(words, args->structureInput, sizeof(words));
        note->transaction=words[6];note->surface=words[7];
        uint64_t metadataBegin = mach_absolute_time();
        IOLockLock(transactionLock);
        bool valid = activeTransaction && words[6] == activeTransaction;
        if (valid) activeTransaction = 0;
        IOLockUnlock(transactionLock);
        static bool capturedOffscreen;
        bool captureOffscreen = words[7] > primarySurfaceID && !capturedOffscreen;
        if (captureOffscreen) capturedOffscreen = true;
        if (captures++ < 4 || captureOffscreen) {
            IOLog("IOS7LAB framebuffer swap-payload valid-transaction=%u words=", (unsigned)valid);
            for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
                IOLog("%08x%s", words[i], i + 1 == sizeof(words) / sizeof(words[0]) ? "\n" : ",");
        }
        if (!valid) {
            note->reason=IOS7LabSwapMetaTransaction;
            noteSwapMetadata(IOS7LabSwapMetaTransaction, 0x3U, 0xffffffffU, words, args,
                metadataBegin, 0, mach_absolute_time(), kIOReturnBadArgument);
            return kIOReturnBadArgument;
        }
        uint32_t metadataReason = 0, metadataMismatch = 0xffffffffU;
        bool originalLayout = ios7lab_fullframe_scanout_layout(words, words[7],
            &metadataReason, &metadataMismatch);
        if (!originalLayout || !ios7lab_primary_ready()) {
            if (originalLayout) metadataReason = IOS7LabSwapMetaPrimaryReady;
            note->reason=metadataReason;note->mismatch=metadataMismatch;
            static unsigned int rejected;
            if (rejected++ < 12)
                IOLog("IOS7LAB framebuffer unsupported transaction=%u surface=%u masks=%x,%x source=%ux%u output=%ux%u\n",
                      words[6], words[7], words[67], words[68], words[13], words[14], words[53], words[54]);
            noteSwapMetadata(metadataReason, originalLayout ? 0xfU : 0x7U, metadataMismatch, words, args,
                metadataBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        uint64_t metadataBackendBegin = mach_absolute_time();
        note->backend=true;
        IOReturn displayed = ios7lab_present_owned_scanout(words[7]);
        uint64_t metadataEnd = mach_absolute_time();
        if (displayed != kIOReturnSuccess) {
            note->reason=IOS7LabSwapMetaBackend;
            noteSwapMetadata(IOS7LabSwapMetaBackend, 0x3fU, 0xffffffffU, words, args,
                metadataBegin, metadataBackendBegin, metadataEnd, displayed);
            return displayed;
        }
        IOLockLock(transactionLock);
        ScanoutCompletion *completion = &completions[words[6] % 32];
        completion->transaction = words[6];
        completion->surface = words[7];
        completion->completed = true; /* CPUconversion + new realMDP31 DMA_P_DONE, not physicalVSYNC. */
        IOLockUnlock(transactionLock);
        static unsigned int presented;
        if (presented++ < 24)
            IOLog("IOS7LAB framebuffer presented transaction=%u surface=%u actualCPU-conversion-MDP31-DMA-done %s\n",
                  words[6], words[7], "BGRA-to-realRGB565-conversion");
        noteSwapMetadata(IOS7LabSwapMetaCompleted, 0x7fU, 0xffffffffU, words, args,
            metadataBegin, metadataBackendBegin, metadataEnd, kIOReturnSuccess);
        note->reason=IOS7LabSwapMetaCompleted;
        return kIOReturnSuccess;
    }
    if (selector == 6 && args->scalarInput && args->scalarInputCount == 3 &&
        !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize) {
        /* Original kern_SwapWait passes uint32 transaction, uint32 options,
         * uint64 timeout. The normal no-timeout call sets the third value0. */
        static unsigned int waitLogs;
        uint64_t transaction = args->scalarInput[0];
        note->transaction=transaction;note->reason=LEO_GRAPHICS_REASON_WAIT;
        uint64_t options = args->scalarInput[1];
        uint64_t timeout = args->scalarInput[2];
        uint64_t metadataWaitBegin = mach_absolute_time();
        if (waitLogs++ < 24)
            IOLog("IOS7LAB framebuffer swap-wait transaction=%llu options=%llu timeout=%llu\n",
                (unsigned long long)transaction, (unsigned long long)options,
                (unsigned long long)timeout);
        /* Original virtual SwapWait compares its completed counter with the
         * requested counter; the initial fence0 is already complete. Only
         * acknowledge this observed initial form while our engine is empty. */
        if (!transaction && options == 0x80000003ULL && !timeout) {
            IOLockLock(transactionLock);
            bool idle = !nextTransaction && !activeTransaction;
            IOLockUnlock(transactionLock);
            static unsigned int initialLogs;
            if (initialLogs++ < 4)
                IOLog("IOS7LAB framebuffer initial-fence no-submission=%u result=%x\n",
                    (unsigned)idle, idle ? kIOReturnSuccess : kIOReturnNotReady);
            note->reason=10;
            noteWaitMetadata(10, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), idle ? kIOReturnSuccess : kIOReturnNotReady);
            return idle ? kIOReturnSuccess : kIOReturnNotReady;
        }
        if (!transaction || transaction > 0xffffffffULL) {
            note->reason=11;
            noteWaitMetadata(11, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnBadArgument);
            return kIOReturnBadArgument;
        }
        /* Current native compositor uses the recorded high-bit wait option
         * and timeout1000. Unsupported option/deadline forms stay rejected. */
        if (options != 0 && options != 0x80000000ULL) {
            note->reason=12;
            noteWaitMetadata(12, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        if (timeout != 0 && timeout != 1000) {
            note->reason=13;
            noteWaitMetadata(13, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        IOLockLock(transactionLock);
        ScanoutCompletion snapshot = completions[(uint32_t)transaction % 32];
        IOLockUnlock(transactionLock);
        note->surface=snapshot.surface;
        if (snapshot.transaction != transaction) {
            note->reason=14;
            noteWaitMetadata(14, 0x3U, transaction, options, timeout,
                snapshot.transaction, snapshot.surface, snapshot.completed,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnNotFound);
            return kIOReturnNotFound;
        }
        if (snapshot.completed) {
            note->reason=15;
            noteWaitMetadata(15, 0x7U, transaction, options, timeout,
                snapshot.transaction, snapshot.surface, snapshot.completed,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnSuccess);
            return kIOReturnSuccess;
        }
        uint64_t metadataWaitBackendBegin = mach_absolute_time();
        IOReturn waited = ios7lab_wait_owned_scanout(snapshot.surface, 1000);
        uint64_t metadataWaitEnd = mach_absolute_time();
        if (waited == kIOReturnSuccess) {
            IOLockLock(transactionLock);
            ScanoutCompletion *completion = &completions[(uint32_t)transaction % 32];
            if (completion->transaction == transaction) completion->completed = true;
            IOLockUnlock(transactionLock);
        }
        static unsigned int resultLogs;
        if (resultLogs++ < 24)
            IOLog("IOS7LAB framebuffer swap-wait recorded DMA-completion transaction=%llu result=%x\n",
                (unsigned long long)transaction, waited);
        note->reason=16;
        noteWaitMetadata(16, 0xfU, transaction, options, timeout,
            snapshot.transaction, snapshot.surface, snapshot.completed,
            metadataWaitBegin, metadataWaitBackendBegin, metadataWaitEnd, waited);
        return waited;
    }
    if (selector == 9 && args->scalarInput && args->scalarInputCount == 2 &&
        !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize) {
        LeoVsyncAction action={LEO_VSYNC_CALLBACK,MACH_PORT_NULL,0,
            (io_user_reference_t)args->scalarInput[1],(io_user_reference_t)args->scalarInput[0],0};
        IOReturn result=vsyncWorkLoop->runAction(&IOS7LabFramebufferClient::vsyncWorkAction,this,&action);
        note->reason=result==kIOReturnNotOpen?LEO_GRAPHICS_REASON_CLOSED:
            result==kIOReturnSuccess?LEO_GRAPHICS_REASON_NONE:LEO_GRAPHICS_REASON_TIMER;
        return result;
    }
    if (args->scalarInputCount || args->structureInputSize || args->structureOutputSize)
        return kIOReturnUnsupported;
    if (selector == 4 && args->scalarOutput && args->scalarOutputCount == 1) {
        IOLockLock(transactionLock);
        if (activeTransaction) {
            IOLockUnlock(transactionLock);note->reason=LEO_GRAPHICS_REASON_BUSY;
            return kIOReturnBusy;
        }
        if (!++nextTransaction) ++nextTransaction;
        activeTransaction = nextTransaction;
        args->scalarOutput[0] = activeTransaction;
        note->transaction=activeTransaction;note->reason=LEO_GRAPHICS_REASON_NONE;
        IOLockUnlock(transactionLock);
        return kIOReturnSuccess;
    }
    if (selector == 3 && args->scalarOutput && args->scalarOutputCount == 1) {
        if (!ios7lab_primary_ready()) return kIOReturnNoMemory;
        args->scalarOutput[0] = primarySurfaceID;
        return kIOReturnSuccess;
    }
    /* Original 7.1.2 GetDisplaySize: two uint64 output values. */
    if (selector == 8 && args->scalarOutput && args->scalarOutputCount == 2) {
        args->scalarOutput[0] = PE_state.video.v_width;
        args->scalarOutput[1] = PE_state.video.v_height;
        return kIOReturnSuccess;
    }
    /* Original GetMainDisplay filters selector18 for the primary display. */
    if (selector == 18 && args->scalarOutput && args->scalarOutputCount == 1) {
        args->scalarOutput[0] = 1;
        return kIOReturnSuccess;
    }
    /* No fabricated surface/connection IDs or dummy successful rendering. */
    note->reason=LEO_GRAPHICS_REASON_UNSUPPORTED;
    return kIOReturnUnsupported;
}

IOReturn IOMobileFramebuffer::newUserClient(task_t task, void *security, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
    static unsigned int logs;
    if (logs++ < 24)
        IOLog("IOS7LAB framebuffer open type=%u\n", (unsigned int)type);
    if (type != 0 || !handler)
        return kIOReturnBadArgument;
    *handler = 0;
    IOS7LabFramebufferClient *client = new IOS7LabFramebufferClient;
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

void ios7leo_register_display(IOService *platform)
{
    if (!ios7leo_display_ready())return;
    if (!PE_state.video.v_baseAddr || !PE_state.video.v_width || !PE_state.video.v_height)
        return;
    primaryLock = IOLockAlloc();
    if (!primaryLock) return;
    uint64_t managed=sane_size>>12,freePages=vm_page_free_count;
    uint64_t guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
    if(freePages<=guard)return;
    uint64_t budget=(managed>>3)*4096U,available=((freePages-guard)/2U)*4096U;
    if(budget>available)budget=available;if(budget>16U*1024U*1024U)budget=16U*1024U*1024U;
    displayBudget=(uint32_t)budget;
    if(displayBudget<offscreenPixelBytes+4096U+65536U || !ios7lab_primary_ready())return;
    IOSurfaceRoot *surfaces = new IOCoreSurfaceRoot;
    IOMobileFramebuffer *display = new AppleCLCD;
    bool surfacesAttached=false,displayAttached=false;
    bool ready=surfaces && display && surfaces->init() && display->init();
    if(ready){surfacesAttached=surfaces->attach(platform);displayAttached=display->attach(platform);ready=surfacesAttached&&displayAttached;}
    if(ready)ready=surfaces->start(platform)&&display->start(platform);
    if(!ready){
        if(displayAttached)display->detach(platform);if(surfacesAttached)surfaces->detach(platform);
        if(display)display->release();if(surfaces)surfaces->release();
        IOLog("Leo display registration failed; no provider published\n");return;
    }
    surfaces->setName("IOCoreSurfaceRoot");surfaces->setProperty("IOS7LabBackend","Leo owned BGRA CPU surface / real MDP RGB565");
    display->setName("IOS7LeoMDP31");display->setProperty("IOS7LabBackend","HTC Leo MDP31/MDDI native RGB565");
    surfaces->registerService();display->registerService();
    IOLog("Leo native display provider published480x800 BGRA row1920 bytes1536000; physicalRGB565 row960 separate; dynamicbudget=%u\n",displayBudget);
    surfaces->release();display->release();
}
#endif

#endif /* Leo-only hardware backend. */
