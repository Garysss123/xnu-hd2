#if !defined(BOARD_CONFIG_QSD8250_LEO)
/* Reference-board input adaptation. QEMU and original iOS UI stay unchanged. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "IOS7LabPS2.h"
extern "C" {
#include <pexpert/pexpert.h>
}

#if BOARD_CONFIG_ARMPBA8
class IOS7LabPointer : public IOService {
    OSDeclareDefaultStructors(IOS7LabPointer)
    IOMemoryDescriptor *device;
    IOMemoryMap *mapping;
    IOLock *lock;
    volatile uint32_t *registers;
    IOS7LabPS2State state;
    bool sendCommand(uint8_t value);
public:
    virtual bool init(OSDictionary *dictionary = 0);
    virtual bool start(IOService *provider);
    virtual void free(void);
    virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
        OSDictionary *properties, IOUserClient **handler);
    IOReturn readSample(IOExternalMethodArguments *args);
};
class IOS7LabPointerClient : public IOUserClient {
    OSDeclareDefaultStructors(IOS7LabPointerClient)
    IOS7LabPointer *pointer;
public:
    virtual bool start(IOService *provider);
    virtual IOReturn clientClose(void);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *, OSObject *, void *);
};
OSDefineMetaClassAndStructors(IOS7LabPointer, IOService)
OSDefineMetaClassAndStructors(IOS7LabPointerClient, IOUserClient)

bool IOS7LabPointer::init(OSDictionary *dictionary)
{
    if (!IOService::init(dictionary)) return false;
    device = 0; mapping = 0; lock = 0; registers = 0;
    bzero(&state, sizeof(state));
    return true;
}
bool IOS7LabPointer::sendCommand(uint8_t value)
{
    for (unsigned n = 0; n < 100; ++n) {
        if (registers[1] & 0x40) {
            registers[2] = value;
            __sync_synchronize();
            for (unsigned reply = 0; reply < 100; ++reply) {
                if (registers[1] & 0x10) return (registers[2] & 255) == 0xfa;
                IOSleep(1);
            }
            return false;
        }
        IOSleep(1);
    }
    return false;
}
bool IOS7LabPointer::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    lock = IOLockAlloc();
    device = IOMemoryDescriptor::withPhysicalAddress(0x10007000, 4096, kIODirectionInOut);
    mapping = device ? device->map(kIOMapInhibitCache) : 0;
    if (!lock || !mapping || !mapping->getAddress()) return false;
    registers = (volatile uint32_t *)mapping->getAddress();
    if ((registers[0xfe0/4] & 255) != 0x50 || (registers[0xfe4/4] & 255) != 0x10 ||
        (registers[0xfe8/4] & 255) != 4 || (registers[0xfec/4] & 255) != 0) {
        registers = 0; // Do not write to an unidentified peripheral during free.
        return false;
    }
    registers[3] = 8;
    registers[0] = 4; // KMI enabled, interrupt masked; bounded client polling.
    __sync_synchronize();
    for (unsigned n = 0; n < 32 && (registers[1] & 0x10); ++n) {
        uint32_t discarded = registers[2];
        (void)discarded;
    }
    if (!sendCommand(0xf6) || !sendCommand(0xf4)) return false;
    setName("IOS7LabPointer");
    setProperty("IOS7LabBackend", "RealView PL050 PS2 mouse");
    IOLog("IOS7LAB pointer PL050 identified, PS2 streaming enabled; native input adapter required\n");
    return true;
}
void IOS7LabPointer::free(void)
{
    if (registers) registers[0] = 0;
    if (mapping) mapping->release();
    if (device) device->release();
    if (lock) IOLockFree(lock);
    IOService::free();
}
IOReturn IOS7LabPointer::readSample(IOExternalMethodArguments *args)
{
    if (!args || args->scalarInputCount || args->structureInputSize ||
        args->scalarOutputCount || !args->structureOutput ||
        args->structureOutputSize < sizeof(IOS7LabPointerSample) ||
        args->structureInputDescriptor || args->structureOutputDescriptor)
        return kIOReturnBadArgument;
    IOLockLock(lock);
    bool available = false;
    for (unsigned n = 0; n < 32 && (registers[1] & 0x10); ++n) {
        uint8_t value = (uint8_t)registers[2];
        if (ios7lab_ps2_byte(&state, value, (uint32_t)PE_state.video.v_width,
                             (uint32_t)PE_state.video.v_height)) {
            available = true;
            break; // Preserve button-down/up ordering, one packet per read.
        }
    }
    IOS7LabPointerSample sample = {(uint32_t)available, state.sequence, state.x, state.y,
        state.buttons, state.packets, state.errors, 0};
    IOLockUnlock(lock);
    memcpy(args->structureOutput, &sample, sizeof(sample));
    args->structureOutputSize = sizeof(sample);
    return kIOReturnSuccess;
}
bool IOS7LabPointerClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    pointer = OSDynamicCast(IOS7LabPointer, provider);
    return pointer != 0;
}
IOReturn IOS7LabPointerClient::clientClose(void)
{
    terminate();
    return kIOReturnSuccess;
}
IOReturn IOS7LabPointerClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!pointer || selector) return kIOReturnUnsupported;
    return pointer->readSample(args);
}
IOReturn IOS7LabPointer::newUserClient(task_t task, void *security, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
    if (type || !handler) return kIOReturnBadArgument;
    *handler = 0;
    IOS7LabPointerClient *client = new IOS7LabPointerClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) { client->release(); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    *handler = client;
    return kIOReturnSuccess;
}
void ios7lab_register_pointer(IOService *platform)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("ios7lab_vmabi", &enabled, sizeof(enabled)) || enabled != 1) return;
    IOS7LabPointer *pointer = new IOS7LabPointer;
    if (!pointer) return;
    bool attached = pointer->init() && pointer->attach(platform);
    if (attached && pointer->start(platform)) pointer->registerService();
    else if (attached) pointer->detach(platform);
    pointer->release();
}
#endif

#endif /* RealView transport is not active in Leo entry-only image. */
