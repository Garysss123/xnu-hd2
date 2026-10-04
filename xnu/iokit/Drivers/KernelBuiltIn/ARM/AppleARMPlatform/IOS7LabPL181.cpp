#if !defined(BOARD_CONFIG_QSD8250_LEO)
/* RealView stock PL181 SD block transport. Staged, not installed.
 * Single-block PIO, original IOStorage completion semantics, no DMA/IRQ path. */
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include "IOS7LabSDProtocol.h"
extern "C" {
#include <pexpert/pexpert.h>
}

class IOS7LabPL181 : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(IOS7LabPL181)
    IOMemoryDescriptor *mmio, *sysctl;
    IOMemoryMap *mmioMap, *sysctlMap;
    volatile UInt32 *regs, *systemRegs;
    IOLock *mutex;
    UInt64 sectorCount;
    UInt32 rca;
    bool highCapacity, csdProtected, initialized, faulted;
    UInt32 readReg(unsigned off) { OSSynchronizeIO(); return regs[off/4]; }
    void writeReg(unsigned off, UInt32 value) { regs[off/4]=value; OSSynchronizeIO(); }
    bool inserted() { OSSynchronizeIO(); return (systemRegs[0x48/4]&1U)!=0; }
    bool protectedMedia() { OSSynchronizeIO(); return csdProtected || (systemRegs[0x48/4]&2U)!=0; }
    IOReturn command(UInt32 index, UInt32 argument, unsigned response, uint32_t out[4]);
    IOReturn cardInit();
    IOReturn ready();
    IOReturn sector(UInt64 block, UInt8 bytes[512], bool writing);
public:
    virtual bool init(OSDictionary *properties=0);
    virtual bool start(IOService *provider);
    virtual void free();
    virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
                                     IOStorageAttributes *, IOStorageCompletion *);
    virtual IOReturn doEjectMedia() { return kIOReturnUnsupported; }
    virtual IOReturn doFormatMedia(UInt64) { return kIOReturnUnsupported; }
    virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const { return 0; }
    virtual IOReturn doLockUnlockMedia(bool) { return kIOReturnUnsupported; }
    virtual IOReturn doSynchronizeCache();
    virtual char *getVendorString();
    virtual char *getProductString();
    virtual char *getRevisionString();
    virtual char *getAdditionalDeviceInfoString();
    virtual IOReturn reportBlockSize(UInt64 *value);
    virtual IOReturn reportMaxValidBlock(UInt64 *value);
    virtual IOReturn reportEjectability(bool *value);
    virtual IOReturn reportLockability(bool *value);
    virtual IOReturn reportRemovability(bool *value);
    virtual IOReturn reportMediaState(bool *present, bool *changed);
    virtual IOReturn reportPollRequirements(bool *required, bool *expensive);
    virtual IOReturn reportWriteProtection(bool *value);
};
OSDefineMetaClassAndStructors(IOS7LabPL181, IOBlockStorageDevice)

/* R1 error bits, excluding CURRENT_STATE/READY_FOR_DATA/APP_CMD. */
static const UInt32 r1Errors=0xfdffe008U;
static const UInt32 dataErrors=(1U<<1)|(1U<<3)|(1U<<4)|(1U<<5);
static const unsigned pollLimit=1000000;

bool IOS7LabPL181::init(OSDictionary *properties)
{
    if (!IOBlockStorageDevice::init(properties)) return false;
    mmio=sysctl=0; mmioMap=sysctlMap=0; regs=systemRegs=0; mutex=0;
    sectorCount=0; rca=0; highCapacity=csdProtected=initialized=faulted=false;
    return true;
}
bool IOS7LabPL181::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;
    mutex=IOLockAlloc();
    mmio=IOMemoryDescriptor::withPhysicalAddress(0x10005000,4096,kIODirectionInOut);
    sysctl=IOMemoryDescriptor::withPhysicalAddress(0x10000000,4096,kIODirectionInOut);
    if (!mutex || !mmio || !sysctl) return false;
    mmioMap=mmio->map(kIOMapInhibitCache); sysctlMap=sysctl->map(kIOMapInhibitCache);
    if (!mmioMap || !sysctlMap) return false;
    regs=(volatile UInt32 *)mmioMap->getVirtualAddress();
    systemRegs=(volatile UInt32 *)sysctlMap->getVirtualAddress();
    if (!regs || !systemRegs) return false;
    const UInt8 expected[8]={0x81,0x11,0x04,0,0x0d,0xf0,0x05,0xb1};
    for (unsigned i=0;i<8;++i) if (readReg(0xfe0+i*4)!=expected[i]) return false;
    writeReg(0x3c,0); writeReg(0x40,0); writeReg(0x2c,0);
    writeReg(0,3); writeReg(4,0xff);
    IOReturn result=cardInit();
    if (result!=kIOReturnSuccess) {
        IOLog("[ios7lab-sd] card-init error=0x%x\n",(unsigned)result); return false;
    }
    initialized=true;
    IOLog("[ios7lab-sd] PL181 sectors=%llu sector-bytes=512 readonly=%u\n",
          (unsigned long long)sectorCount,(unsigned)protectedMedia());
    return true;
}
void IOS7LabPL181::free()
{
    if (regs) { writeReg(0x3c,0); writeReg(0x40,0); writeReg(0x2c,0); }
    if (mmioMap) mmioMap->release(); if (sysctlMap) sysctlMap->release();
    if (mmio) mmio->release(); if (sysctl) sysctl->release();
    if (mutex) IOLockFree(mutex);
    IOBlockStorageDevice::free();
}
IOReturn IOS7LabPL181::command(UInt32 index, UInt32 argument, unsigned response, uint32_t out[4])
{
    if (!inserted()) return kIOReturnNoMedia;
    writeReg(0x38,0xc5); /* command completion/error only, preserve data status */
    writeReg(8,argument);
    writeReg(0xc,index|0x400U|(response?0x40U:0U)|(response==2?0x80U:0U));
    for (unsigned n=0;n<pollLimit;++n) {
        UInt32 status=readReg(0x34);
        if (status&(1U<<2)) return kIOReturnTimeout;
        if (status&1U) return kIOReturnIOError;
        if (status&(response?(1U<<6):(1U<<7))) {
            if (out) for (unsigned i=0;i<4;++i) out[i]=readReg(0x14+4*i);
            return kIOReturnSuccess;
        }
        IODelay(1);
    }
    return kIOReturnTimeout;
}
IOReturn IOS7LabPL181::cardInit()
{
    uint32_t response[4]={0,0,0,0};
    IOReturn result=command(0,0,0,0);
    if (result!=kIOReturnSuccess) return result;
    result=command(8,0x1aa,1,response);
    bool version2=result==kIOReturnSuccess;
    if (version2 && response[0]!=0x1aa) return kIOReturnUnsupported;
    if (!version2 && result!=kIOReturnTimeout) return result;
    bool powered=false;
    for (unsigned attempt=0;attempt<100;++attempt) {
        result=command(55,0,1,response);
        if (result!=kIOReturnSuccess) return result;
        if ((response[0]&r1Errors) || !(response[0]&(1U<<5))) return kIOReturnIOError;
        result=command(41,0x00ff8000U|(version2?0x40000000U:0),1,response);
        if (result!=kIOReturnSuccess) return result;
        if (response[0]&0x80000000U) { powered=true; highCapacity=(response[0]&0x40000000U)!=0; break; }
        IOSleep(10);
    }
    if (!powered) return kIOReturnTimeout;
    result=command(2,0,2,response); if (result!=kIOReturnSuccess) return result;
    result=command(3,0,1,response); if (result!=kIOReturnSuccess) return result;
    if ((response[0]&0xe000U) || !(response[0]&0xffff0000U)) return kIOReturnIOError;
    rca=response[0]&0xffff0000U;
    result=command(9,rca,2,response); if (result!=kIOReturnSuccess) return result;
    int writeProtected=0; uint64_t count=0;
    if (!ios7lab_sd_capacity(response,highCapacity,&count,&writeProtected)) return kIOReturnUnsupported;
    sectorCount=count; csdProtected=writeProtected!=0;
    result=command(7,rca,1,response); if (result!=kIOReturnSuccess) return result;
    if (response[0]&r1Errors) return kIOReturnIOError;
    if (!highCapacity) {
        result=command(16,512,1,response); if (result!=kIOReturnSuccess) return result;
        if (response[0]&r1Errors) return kIOReturnIOError;
    }
    return ready();
}
IOReturn IOS7LabPL181::ready()
{
    uint32_t response[4];
    for (unsigned attempt=0;attempt<100;++attempt) {
        IOReturn result=command(13,rca,1,response);
        if (result!=kIOReturnSuccess) return result;
        if (response[0]&r1Errors) return kIOReturnIOError;
        if ((response[0]&(1U<<8)) && ((response[0]>>9)&15U)==4U) return kIOReturnSuccess;
        IOSleep(1);
    }
    return kIOReturnTimeout;
}
IOReturn IOS7LabPL181::sector(UInt64 block, UInt8 bytes[512], bool writing)
{
    uint32_t argument=0,response[4];
    if (!ios7lab_sd_command_address(block,highCapacity,&argument)) return kIOReturnBadArgument;
    if (!inserted()) return kIOReturnNoMedia;
    if (writing && protectedMedia()) return kIOReturnNotWritable;
    writeReg(0x2c,0); writeReg(0x38,0x7ff); writeReg(0x24,0xffffffffU); writeReg(0x28,512);
    writeReg(0x2c,1U|(writing?0U:2U)|(9U<<4));
    IOReturn result=command(writing?24:17,argument,1,response);
    if (result==kIOReturnSuccess && (response[0]&r1Errors)) result=kIOReturnIOError;
    unsigned words=0;
    for (unsigned n=0;result==kIOReturnSuccess && n<pollLimit;++n) {
        UInt32 status=readReg(0x34); /* also advances stock PL181 FIFO refill */
        if (!inserted()) { result=kIOReturnNoMedia; break; }
        if (status&dataErrors) { result=(status&(1U<<3))?kIOReturnTimeout:kIOReturnIOError; break; }
        if (words<128 && (writing?!(status&(1U<<16)):(status&(1U<<21)))) {
            unsigned p=words*4;
            if (writing) writeReg(0x80,(UInt32)bytes[p]|((UInt32)bytes[p+1]<<8)|
                                      ((UInt32)bytes[p+2]<<16)|((UInt32)bytes[p+3]<<24));
            else { UInt32 word=readReg(0x80); for (unsigned b=0;b<4;++b) bytes[p+b]=(UInt8)(word>>(b*8)); }
            ++words;
        }
        if (words==128 && (status&(1U<<8)) && (status&(1U<<10)) && readReg(0x30)==0) break;
        if (n==pollLimit-1) result=kIOReturnTimeout;
        IODelay(1);
    }
    writeReg(0x2c,0);
    if (result==kIOReturnSuccess && words!=128) result=kIOReturnUnderrun;
    if (result==kIOReturnSuccess && writing) result=ready();
    return result;
}
IOReturn IOS7LabPL181::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                     IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    UInt64 bytes=0;
    if (!buffer || !completion || !completion->action || !initialized ||
        !ios7lab_sd_request_range(sectorCount,block,count,buffer->getLength(),&bytes)) return kIOReturnBadArgument;
    IODirection direction=buffer->getDirection();
    if (direction!=kIODirectionIn && direction!=kIODirectionOut) return kIOReturnBadArgument;
    if (attributes && (attributes->options || attributes->reserved0032 || attributes->reserved0064)) return kIOReturnUnsupported;
    IOStorageCompletion copied=*completion;
    bool writing=direction==kIODirectionOut;
    UInt64 completed=0; IOReturn result=kIOReturnSuccess; UInt8 data[512];
    IOLockLock(mutex);
    /* A queued caller may have validated before an earlier controller fault.
     * PL181 has no FIFO reset register: do not reuse retained FIFO/card state. */
    if (faulted) { IOLockUnlock(mutex); return kIOReturnNotReady; }
    for (UInt64 index=0;index<count;++index) {
        if (writing && buffer->readBytes((IOByteCount)completed,data,512)!=512) { result=kIOReturnUnderrun; break; }
        result=sector(block+index,data,writing);
        if (result!=kIOReturnSuccess) {
            if (result!=kIOReturnNotWritable && result!=kIOReturnBadArgument) faulted=true;
            break;
        }
        if (!writing && buffer->writeBytes((IOByteCount)completed,data,512)!=512) { result=kIOReturnUnderrun; break; }
        completed+=512;
    }
    IOLockUnlock(mutex);
    IOStorage::complete(&copied,result,completed);
    return kIOReturnSuccess; /* accepted, completed exactly once above */
}
IOReturn IOS7LabPL181::doSynchronizeCache()
{
    if (!initialized) return kIOReturnNotReady;
    IOLockLock(mutex);
    if (faulted) { IOLockUnlock(mutex); return kIOReturnNotReady; }
    IOReturn result=ready();
    if (result!=kIOReturnSuccess) faulted=true;
    IOLockUnlock(mutex);
    /* SD ready status only. Host backend uses standard cache=writethrough;
     * neither queue completion nor this status asserts a host fsync by itself. */
    return result;
}
char *IOS7LabPL181::getVendorString() { static char text[]="QEMU"; return text; }
char *IOS7LabPL181::getProductString() { static char text[]="PL181 SD"; return text; }
char *IOS7LabPL181::getRevisionString() { static char text[]="PIO1"; return text; }
char *IOS7LabPL181::getAdditionalDeviceInfoString() { static char text[]="RealView PB-A8"; return text; }
IOReturn IOS7LabPL181::reportBlockSize(UInt64 *value) { if (!value) return kIOReturnBadArgument; *value=512; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportMaxValidBlock(UInt64 *value) { if (!value || !sectorCount) return kIOReturnNotReady; *value=sectorCount-1; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportEjectability(bool *value) { if (!value) return kIOReturnBadArgument; *value=false; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportLockability(bool *value) { if (!value) return kIOReturnBadArgument; *value=false; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportRemovability(bool *value) { if (!value) return kIOReturnBadArgument; *value=true; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportMediaState(bool *present, bool *changed) { if (!present || !changed) return kIOReturnBadArgument; *present=inserted(); *changed=false; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportPollRequirements(bool *required, bool *expensive) { if (!required || !expensive) return kIOReturnBadArgument; *required=true; *expensive=false; return kIOReturnSuccess; }
IOReturn IOS7LabPL181::reportWriteProtection(bool *value) { if (!value) return kIOReturnBadArgument; *value=protectedMedia(); return kIOReturnSuccess; }

#include "IOS7LabStoragePersonalities.h"
void ios7lab_register_sd(IOService *platform)
{
    int enabled=0;
    if (!PE_parse_boot_argn("ios7lab_sdroot",&enabled,sizeof(enabled)) || enabled!=1) return;
    OSObject *decoded=OSUnserializeXML(ios7labStoragePersonalities);
    OSArray *drivers=OSDynamicCast(OSArray,decoded);
    if (!drivers || !gIOCatalogue->addDrivers(drivers,false)) {
        IOLog("[ios7lab-sd] catalogue registration failed\n"); if (decoded) decoded->release(); return;
    }
    decoded->release();
    IOS7LabPL181 *device=new IOS7LabPL181;
    if (!device) return;
    bool attached=device->init() && device->attach(platform);
    if (attached && device->start(platform)) device->registerService();
    else if (attached) device->detach(platform);
    device->release();
}

#endif /* RealView transport is not active in Leo entry-only image. */
