#if !defined(BOARD_CONFIG_QSD8250_LEO)
/* Isolated ARMPBA8 display discovery adapter; not an HD2 driver. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "IOS7LabSurfaceGeometry.h"
extern "C" {
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
extern void task_reference(task_t);
extern void task_deallocate(task_t);
extern int ios7lab_realview_present_framebuffer(uint32_t physical);
extern int ios7lab_realview_framebuffer_vblank(uint32_t physical);
}

#if BOARD_CONFIG_ARMPBA8
static IOLock *primaryLock;
static IOMemoryDescriptor *primaryPixels;
static IOBufferMemoryDescriptor *primaryControl;
static uint32_t primaryBytes;
static const uint32_t primarySurfaceID = 1;
static uint32_t activeScanoutSurfaceID = primarySurfaceID;
enum { IOS7LAB_MAX_OFFSCREEN = 64, IOS7LAB_SCANOUT_SLOTS = 8 };

struct IOS7LabOffscreenSurface {
    uint32_t id;
    uint32_t references;
    uint32_t width, height, rowBytes, allocationSize, pixelFormat, scanoutPhysical;
    bool preparedUserBuffer;
    IOMemoryDescriptor *pixels;
    IOMemoryDescriptor *control;
    IOMemoryMap *kernelPixels;
    IOMemoryMap *kernelControl;
};

static IOS7LabOffscreenSurface offscreenSurfaces[IOS7LAB_MAX_OFFSCREEN];
static uint32_t nextOffscreenSurfaceID = 1;
static uint32_t offscreenHeapBytes;
static const uint32_t offscreenHeapLimit = 64U * 1024 * 1024;
static const uint32_t offscreenPoolBase = 0x3e000000U;
static const uint32_t offscreenSlotBytes = 0x301000U;
static const uint32_t offscreenPixelBytes = 0x258000U;
static const uint32_t offscreenControlOffset = 0x300000U;

static const char ios7lab_fullscreen_surface_request[] =
    "<dict><key>IOSurfaceWidth</key><integer size=\"32\">0x280</integer>"
    "<key>IOSurfaceMemoryRegion</key><string>PurpleGfxMem</string>"
    "<key>IOSurfaceIsGlobal</key><true/>"
    "<key>IOSurfaceBytesPerRow</key><integer size=\"32\">0xa00</integer>"
    "<key>IOSurfacePixelFormat</key><integer size=\"32\">0x42475241</integer>"
    "<key>IOSurfaceBytesPerElement</key><integer size=\"32\">0x4</integer>"
    "<key>IOSurfaceCacheMode</key><integer size=\"32\">0x400</integer>"
    "<key>IOSurfaceAllocSize</key><integer size=\"32\">0x258000</integer>"
    "<key>IOSurfaceHeight</key><integer size=\"32\">0x3c0</integer></dict>";

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
    memcpy(expected, words, 4 * sizeof(uint32_t)); /* Original frame timestamps. */
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
    uint64_t physical = PE_state.video.v_baseAddr;
    uint64_t bytes = (uint64_t)PE_state.video.v_rowBytes * PE_state.video.v_height;
    if (PE_state.video.v_depth != 32 || !PE_state.video.v_width || !PE_state.video.v_height ||
        PE_state.video.v_width > 2048 || PE_state.video.v_height > 2048 ||
        PE_state.video.v_rowBytes < PE_state.video.v_width * 4 || bytes > 16 * 1024 * 1024 ||
        physical < 0x70000000ULL || physical + bytes > 0x90000000ULL) {
        IOLog("IOS7LAB primary surface rejected framebuffer extent base=%lx bytes=%lu\n",
              PE_state.video.v_baseAddr, (unsigned long)bytes);
        IOLockUnlock(primaryLock);
        return false;
    }
    IOMemoryDescriptor *pixels = IOMemoryDescriptor::withPhysicalAddress(
        (IOPhysicalAddress)physical, (IOByteCount)bytes, kIODirectionInOut);
    IOBufferMemoryDescriptor *control = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryKernelUserShared, 4096, 4096);
    if (!pixels || !control || !control->getBytesNoCopy()) {
        if (pixels) pixels->release();
        if (control) control->release();
        IOLockUnlock(primaryLock);
        return false;
    }
    bzero(control->getBytesNoCopy(), 4096);
    ((uint32_t *)control->getBytesNoCopy())[2] = 1; /* Initial content generation. */
    primaryPixels = pixels;
    primaryControl = control;
    primaryBytes = (uint32_t)bytes;
    IOLog("IOS7LAB primary surface id=1 actual-PL111 physical=%lx bytes=%u\n",
          (unsigned long)physical, primaryBytes);
    IOLockUnlock(primaryLock);
    return true;
}

static IOReturn ios7lab_present_owned_scanout(uint32_t id)
{
    /* Resolve only an owned full-size surface. The active scanout keeps its
     * slot alive even when its last client releases it. No pixel copy: the
     * native software renderer also draws directly into the default backing. */
    IOReturn result = kIOReturnNotFound;
    IOS7LabOffscreenSurface retired;
    bzero(&retired, sizeof(retired));
    IOLockLock(primaryLock);
    uint32_t physical = 0;
    if (id == primarySurfaceID && primaryPixels)
        physical = (uint32_t)PE_state.video.v_baseAddr;
    else if (id > primarySurfaceID &&
               PE_state.video.v_width == 640 && PE_state.video.v_height == 960 &&
               PE_state.video.v_rowBytes == 640 * 4 &&
               primaryBytes == offscreenPixelBytes) {
        for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
            IOS7LabOffscreenSurface *surface = &offscreenSurfaces[i];
            if (surface->id != id) continue;
            if ((surface->references || id == activeScanoutSurfaceID) &&
                surface->pixels && surface->pixels->getLength() >= primaryBytes &&
                surface->width == 640 && surface->height == 960 &&
                surface->rowBytes == 2560 && surface->pixelFormat == 0x42475241U)
                physical = surface->scanoutPhysical;
            break;
        }
    }
    if (physical) {
        result = ios7lab_realview_present_framebuffer(physical) ?
            kIOReturnSuccess : kIOReturnIOError;
        if (result == kIOReturnSuccess && id != activeScanoutSurfaceID) {
            uint32_t previous = activeScanoutSurfaceID;
            activeScanoutSurfaceID = id;
            for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
                IOS7LabOffscreenSurface *surface = &offscreenSurfaces[i];
                if (surface->id == previous && !surface->references) {
                    retired = *surface;
                    if (!surface->scanoutPhysical) offscreenHeapBytes -= surface->allocationSize;
                    bzero(surface, sizeof(*surface));
                    break;
                }
            }
        }
    }
    /* A fence records completion of its own submitted frame, even after the
     * client selects a later surface. Hold both backing lifetimes until the
     * new frame observes a fresh matching hardware edge. */
    if (result == kIOReturnSuccess) {
        result = kIOReturnTimeout;
        for (unsigned poll = 0; poll < 1000; ++poll) {
            if (ios7lab_realview_framebuffer_vblank(physical)) {
                result = kIOReturnSuccess;
                break;
            }
            IOSleep(1);
        }
    }
    IOLockUnlock(primaryLock);
    if (retired.kernelPixels) retired.kernelPixels->release();
    if (retired.kernelControl) retired.kernelControl->release();
    if (retired.pixels) {
        if (retired.preparedUserBuffer) retired.pixels->complete(kIODirectionInOut);
        retired.pixels->release();
    }
    if (retired.control) retired.control->release();
    return result;
}

static IOReturn ios7lab_wait_owned_scanout(uint32_t id, unsigned maximumPolls = 100)
{
    IOReturn result = kIOReturnNotFound;
    IOLockLock(primaryLock);
    uint32_t physical = 0;
    if (id == activeScanoutSurfaceID) {
        if (id == primarySurfaceID && primaryPixels)
            physical = (uint32_t)PE_state.video.v_baseAddr;
        else for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
            if (offscreenSurfaces[i].id == id) {
                physical = offscreenSurfaces[i].scanoutPhysical;
                break;
            }
        }
    }
    if (physical) {
        result = kIOReturnTimeout;
        /* Hold the selected backing and serialize new commits while waiting.
         * Sleeping yields the CPU; a missing hardware edge never succeeds. */
        for (unsigned poll = 0; poll < maximumPolls; ++poll) {
            if (ios7lab_realview_framebuffer_vblank(physical)) {
                result = kIOReturnSuccess;
                break;
            }
            IOSleep(1);
        }
    }
    IOLockUnlock(primaryLock);
    return result;
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
            if (!surface->scanoutPhysical) offscreenHeapBytes -= surface->allocationSize;
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

    IOS7LabSimpleSurfaceRequest geometry = {0, 0, 640, 960, 0x42475241U, 4, 2560, offscreenPixelBytes};
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
    bool fullFrame = !userBuffer && geometry->width == 640 && geometry->height == 960 &&
        geometry->bytesPerRow == 2560 && geometry->pixelFormat == 0x42475241U &&
        geometry->allocationSize <= offscreenControlOffset;
    IOLockLock(primaryLock);
    uint32_t physical = 0;
    if (fullFrame) {
        for (unsigned slot = 0; slot < IOS7LAB_SCANOUT_SLOTS; ++slot) {
            uint32_t candidate = offscreenPoolBase + slot * offscreenSlotBytes;
            bool busy = false;
            for (unsigned j = 0; j < IOS7LAB_MAX_OFFSCREEN; ++j)
                if (offscreenSurfaces[j].id && offscreenSurfaces[j].scanoutPhysical == candidate) busy = true;
            if (!busy) { physical = candidate; break; }
        }
        if (!physical) { IOLockUnlock(primaryLock); return kIOReturnNoResources; }
    } else if (geometry->allocationSize > offscreenHeapLimit - offscreenHeapBytes) {
        IOLockUnlock(primaryLock); return kIOReturnNoMemory;
    }
    for (unsigned i = 0; i < IOS7LAB_MAX_OFFSCREEN; ++i) {
        if (offscreenSurfaces[i].id) continue;
        IOMemoryDescriptor *pixels = userBuffer ? IOMemoryDescriptor::withAddressRange(
            (mach_vm_address_t)geometry->memoryAddressLow, geometry->allocationSize,
            kIODirectionInOut, ownerTask) : fullFrame ? IOMemoryDescriptor::withPhysicalAddress(
            (IOPhysicalAddress)physical, geometry->allocationSize, kIODirectionInOut) :
            IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared,
                                                 geometry->allocationSize, 4096);
        IOMemoryDescriptor *control = fullFrame ? IOMemoryDescriptor::withPhysicalAddress(
            (IOPhysicalAddress)(physical + offscreenControlOffset), 4096, kIODirectionInOut) :
            IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, 4096, 4096);
        bool preparedUserBuffer = userBuffer && pixels && pixels->prepare(kIODirectionInOut) == kIOReturnSuccess;
        if (userBuffer && !preparedUserBuffer) {
            if (pixels) pixels->release();
            if (control) control->release();
            break;
        }
        IOMemoryMap *kernelPixels = pixels ? pixels->map(kIOMapCopybackCache) : 0;
        IOMemoryMap *kernelControl = control ? control->map(kIOMapCopybackCache) : 0;
        if (!pixels || !control || !kernelPixels || !kernelControl ||
            !kernelPixels->getAddress() || !kernelControl->getAddress()) {
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
        offscreenSurfaces[i].pixelFormat = geometry->pixelFormat;
        offscreenSurfaces[i].scanoutPhysical = physical;
        offscreenSurfaces[i].preparedUserBuffer = preparedUserBuffer;
        offscreenSurfaces[i].pixels = pixels;
        offscreenSurfaces[i].control = control;
        offscreenSurfaces[i].kernelPixels = kernelPixels;
        offscreenSurfaces[i].kernelControl = kernelControl;
        if (!fullFrame) offscreenHeapBytes += geometry->allocationSize;
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
    info[8] = (uint32_t)PE_state.video.v_rowBytes;
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
    uint64_t vsyncCount;
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
    vsyncCount = 0;
    if (!IOUserClient::initWithTask(task, security, type, properties))
        return false;
    transactionLock = IOLockAlloc();
    vsyncWorkLoop = IOWorkLoop::workLoop();
    vsyncTimer = IOTimerEventSource::timerEventSource(this,
        &IOS7LabFramebufferClient::vsyncTimerFired);
    if (!transactionLock || !vsyncWorkLoop || !vsyncTimer ||
        vsyncWorkLoop->addEventSource(vsyncTimer) != kIOReturnSuccess)
        return false;
    return true;
}

void IOS7LabFramebufferClient::stopVsync(void)
{
    if (!transactionLock) return;
    IOLockLock(transactionLock);
    vsyncEnabled = false;
    if (vsyncTimer) vsyncTimer->cancelTimeout();
    IOLockUnlock(transactionLock);
}

void IOS7LabFramebufferClient::vsyncTimerFired(OSObject *owner,
    IOTimerEventSource *sender)
{
    IOS7LabFramebufferClient *client = OSDynamicCast(IOS7LabFramebufferClient, owner);
    if (client) client->emitVsync(sender);
}

void IOS7LabFramebufferClient::emitVsync(IOTimerEventSource *sender)
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
    IOLockLock(transactionLock);
    if (!vsyncEnabled || !vsyncPort || !vsyncCallback) {
        IOLockUnlock(transactionLock);
        return;
    }
    bzero(reference, sizeof(reference));
    setAsyncReference64(reference, vsyncPort, vsyncCallback, vsyncRefcon);
    uint64_t emittedCount = ++vsyncCount;
    IOLockUnlock(transactionLock);
    IOReturn result = sendAsyncResult64(reference, kIOReturnSuccess, values, 6);
    static unsigned int logs;
    if (logs++ < 24)
        IOLog("IOS7LAB vsync notification count=%llu result=%x now=%llu interval=%llu\n",
              (unsigned long long)vsyncCount, result, (unsigned long long)now,
              (unsigned long long)interval);
    /* Observe only the existing real send-failure branch. No retry, new
     * clock query, callback/event read, port inspection or scheduling change. */
    if (result != kIOReturnSuccess) {
        static volatile unsigned int failureRecords;
        unsigned int slot = failureRecords;
        if (slot < 8 && __sync_bool_compare_and_swap(&failureRecords, slot, slot + 1))
            IOLog("IOS7LAB vsync-send failure record=%u count=%llu result=%x now=%llu interval=%llu\n",
                  slot + 1, (unsigned long long)emittedCount, result,
                  (unsigned long long)now, (unsigned long long)interval);
    }
    IOLockLock(transactionLock);
    if (vsyncEnabled && result == kIOReturnSuccess) sender->setTimeoutUS(16667);
    else vsyncEnabled = false;
    IOLockUnlock(transactionLock);
}

IOReturn IOS7LabFramebufferClient::registerNotificationPort(mach_port_t port,
    UInt32 type, io_user_reference_t refCon)
{
    if (type != 0) return kIOReturnUnsupported;
    mach_port_t oldPort;
    IOLockLock(transactionLock);
    oldPort = vsyncPort;
    vsyncPort = port;
    IOLockUnlock(transactionLock);
    if (oldPort) releaseNotificationPort(oldPort);
    IOLog("IOS7LAB framebuffer notification-port type=0 port=%x refcon=%llx\n",
          port, refCon);
    return kIOReturnSuccess;
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
    static unsigned int logs;
    if (!args)
        return kIOReturnBadArgument;
    if (logs++ < 128)
        IOLog("IOS7LAB framebuffer method=%u scalar-in=%u struct-in=%u scalar-out=%u struct-out=%u\n",
              selector, args->scalarInputCount, args->structureInputSize,
              args->scalarOutputCount, args->structureOutputSize);
    if (args->structureInputDescriptor || args->structureOutputDescriptor)
        return kIOReturnUnsupported;
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
            noteSwapMetadata(IOS7LabSwapMetaTransaction, 0x3U, 0xffffffffU, words, args,
                metadataBegin, 0, mach_absolute_time(), kIOReturnBadArgument);
            return kIOReturnBadArgument;
        }
        uint32_t metadataReason = 0, metadataMismatch = 0xffffffffU;
        bool originalLayout = ios7lab_fullframe_scanout_layout(words, words[7],
            &metadataReason, &metadataMismatch);
        if (!originalLayout || !ios7lab_primary_ready()) {
            if (originalLayout) metadataReason = IOS7LabSwapMetaPrimaryReady;
            static unsigned int rejected;
            if (rejected++ < 12)
                IOLog("IOS7LAB framebuffer unsupported transaction=%u surface=%u masks=%x,%x source=%ux%u output=%ux%u\n",
                      words[6], words[7], words[67], words[68], words[13], words[14], words[53], words[54]);
            noteSwapMetadata(metadataReason, originalLayout ? 0xfU : 0x7U, metadataMismatch, words, args,
                metadataBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        uint64_t metadataBackendBegin = mach_absolute_time();
        IOReturn displayed = ios7lab_present_owned_scanout(words[7]);
        uint64_t metadataEnd = mach_absolute_time();
        if (displayed != kIOReturnSuccess) {
            noteSwapMetadata(IOS7LabSwapMetaBackend, 0x3fU, 0xffffffffU, words, args,
                metadataBegin, metadataBackendBegin, metadataEnd, displayed);
            return displayed;
        }
        IOLockLock(transactionLock);
        ScanoutCompletion *completion = &completions[words[6] % 32];
        completion->transaction = words[6];
        completion->surface = words[7];
        completion->completed = true; /* Submission observed its own hardware edge. */
        IOLockUnlock(transactionLock);
        static unsigned int presented;
        if (presented++ < 24)
            IOLog("IOS7LAB framebuffer presented transaction=%u surface=%u actual-PL111-register-readback %s\n",
                  words[6], words[7], "direct-owned-backing-pixels-unmodified");
        noteSwapMetadata(IOS7LabSwapMetaCompleted, 0x7fU, 0xffffffffU, words, args,
            metadataBegin, metadataBackendBegin, metadataEnd, kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    if (selector == 6 && args->scalarInput && args->scalarInputCount == 3 &&
        !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize) {
        /* Original kern_SwapWait passes uint32 transaction, uint32 options,
         * uint64 timeout. The normal no-timeout call sets the third value0. */
        static unsigned int waitLogs;
        uint64_t transaction = args->scalarInput[0];
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
            noteWaitMetadata(10, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), idle ? kIOReturnSuccess : kIOReturnNotReady);
            return idle ? kIOReturnSuccess : kIOReturnNotReady;
        }
        if (!transaction || transaction > 0xffffffffULL) {
            noteWaitMetadata(11, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnBadArgument);
            return kIOReturnBadArgument;
        }
        /* Current native compositor uses the recorded high-bit wait option
         * and timeout1000. Unsupported option/deadline forms stay rejected. */
        if (options != 0 && options != 0x80000000ULL) {
            noteWaitMetadata(12, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        if (timeout != 0 && timeout != 1000) {
            noteWaitMetadata(13, 0x1U, transaction, options, timeout, 0, 0, false,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnUnsupported);
            return kIOReturnUnsupported;
        }
        IOLockLock(transactionLock);
        ScanoutCompletion snapshot = completions[(uint32_t)transaction % 32];
        IOLockUnlock(transactionLock);
        if (snapshot.transaction != transaction) {
            noteWaitMetadata(14, 0x3U, transaction, options, timeout,
                snapshot.transaction, snapshot.surface, snapshot.completed,
                metadataWaitBegin, 0, mach_absolute_time(), kIOReturnNotFound);
            return kIOReturnNotFound;
        }
        if (snapshot.completed) {
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
            IOLog("IOS7LAB framebuffer swap-wait hardware-edge transaction=%llu result=%x\n",
                (unsigned long long)transaction, waited);
        noteWaitMetadata(16, 0xfU, transaction, options, timeout,
            snapshot.transaction, snapshot.surface, snapshot.completed,
            metadataWaitBegin, metadataWaitBackendBegin, metadataWaitEnd, waited);
        return waited;
    }
    if (selector == 9 && args->scalarInput && args->scalarInputCount == 2 &&
        !args->structureInputSize && !args->scalarOutputCount && !args->structureOutputSize) {
        IOLockLock(transactionLock);
        vsyncCallback = (io_user_reference_t)args->scalarInput[0];
        vsyncRefcon = (io_user_reference_t)args->scalarInput[1];
        vsyncEnabled = vsyncPort && vsyncCallback;
        bool enable = vsyncEnabled;
        IOLockUnlock(transactionLock);
        if (enable) vsyncTimer->setTimeoutUS(16667);
        else stopVsync();
        IOLog("IOS7LAB framebuffer vsync %s callback=%llx refcon=%llx\n",
              enable ? "enabled" : "disabled", vsyncCallback, vsyncRefcon);
        return kIOReturnSuccess;
    }
    if (args->scalarInputCount || args->structureInputSize || args->structureOutputSize)
        return kIOReturnUnsupported;
    if (selector == 4 && args->scalarOutput && args->scalarOutputCount == 1) {
        IOLockLock(transactionLock);
        if (activeTransaction) {
            IOLockUnlock(transactionLock);
            return kIOReturnBusy;
        }
        if (!++nextTransaction) ++nextTransaction;
        activeTransaction = nextTransaction;
        args->scalarOutput[0] = activeTransaction;
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

void ios7lab_register_display(IOService *platform)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("ios7lab_vmabi", &enabled, sizeof(enabled)) || enabled != 1)
        return;
    if (!PE_state.video.v_baseAddr || !PE_state.video.v_width || !PE_state.video.v_height)
        return;
    primaryLock = IOLockAlloc();
    if (!primaryLock) return;
    IOSurfaceRoot *surfaces = new IOCoreSurfaceRoot;
    if (surfaces) {
        bool initialized = surfaces->init();
        bool attached = initialized && surfaces->attach(platform);
        if (attached && surfaces->start(platform)) {
            surfaces->setName("IOCoreSurfaceRoot");
            surfaces->setProperty("IOS7LabBackend", "QEMU PL111 primary CPU surface");
            surfaces->registerService();
            IOLog("IOS7LAB IOSurfaceRoot primary CPU backing published\n");
        } else if (attached) surfaces->detach(platform);
        surfaces->release();
    }
    IOMobileFramebuffer *display = new AppleCLCD;
    if (!display || !display->init()) {
        if (display) display->release();
        return;
    }
    display->setName("IOS7LabPL111");
    display->setProperty("IOS7LabBackend", "QEMU RealView PL111");
    if (!display->attach(platform)) {
        display->release();
        return;
    }
    if (!display->start(platform)) {
        display->detach(platform);
        display->release();
        return;
    }
    display->registerService();
    IOLog("IOS7LAB framebuffer discovery service registered: actual %lux%lu rowbytes=%lu depth=%lu; surfaces pending\n",
          PE_state.video.v_width, PE_state.video.v_height,
          PE_state.video.v_rowBytes, PE_state.video.v_depth);
    display->release();
}
#endif

#endif /* RealView transport is not active in Leo entry-only image. */
