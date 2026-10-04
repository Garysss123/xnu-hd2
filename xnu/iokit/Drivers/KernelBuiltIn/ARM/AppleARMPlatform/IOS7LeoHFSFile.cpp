#if defined(BOARD_CONFIG_QSD8250_LEO)
/* Real readonly SD-backed file media. Original009 PAL/adapter are reused intact. */
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/debug.h>
#include "fat_locator.h"
#include "leo_cow.h"
#include "leo_read_cache.h"
#include <mach/vm_param.h>
/* Exact counter declarations from osfmk/vm/vm_page.h, lines 411/429/443.
 * That private page-layout header is not exported to IOKit. */
extern "C" {
extern unsigned int vm_page_free_count;
extern unsigned int vm_page_free_target;
extern unsigned int vm_page_free_reserved;
}
#define LEO_ROOT_FILE "IOS7/ROOTFS.HFS"
#define LEO_ROOT_TAG "IOS7LeoRootFile"
#define LEO_EXTENT_CAP 4096U
enum RootPhase { NotEntered,WaitingRaw,RawOpened,LocatingFile,ValidatedHFS,
 MediaCreated,MediaRegistered,RootSelected,StorageFailed };
struct RootStatus {
 unsigned phase,raw_open,read0_ok,file_validated,media_created,media_registered,root_selected;
 unsigned reads,major,minor;int locator_error;IOReturn io_error;
 unsigned cow_ready,cow_cap,cow_used,cow_dirty,cow_writes,cow_syncs;IOReturn cow_error;
 UInt64 card_sectors,file_bytes;char bsd_name[20];
 UInt64 cache_hits,cache_misses,cache_base_reads;UInt32 cache_bytes,cache_charge_pages;
};
class IOS7LeoHFSFile;
static IOS7LeoHFSFile *cowStatusOwner;
static RootStatus rootStatus;
static IOSimpleLock *statusLock;
static void phase(unsigned value,int locator=0,IOReturn io=kIOReturnSuccess)
{
 if(!statusLock)return;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 if(rootStatus.phase!=StorageFailed&&(value>=rootStatus.phase))rootStatus.phase=value;
 if(locator&&!rootStatus.locator_error)rootStatus.locator_error=locator;
 if(io!=kIOReturnSuccess&&rootStatus.io_error==kIOReturnSuccess)rootStatus.io_error=io;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
}
extern "C" void ios7leo_root_file_status(void)
{
 if(!statusLock){IOLog("Leo storage: NOT_ENTERED (no file mapper preparation observed)\n");return;}
 RootStatus s;IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 s=rootStatus;IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
 IOLog("Leo storage: phase=%u raw-open=%u read0=%u reads=%u locator=%d io=%08x\n",
  s.phase,s.raw_open,s.read0_ok,s.reads,s.locator_error,(unsigned)s.io_error);
 IOLog("Leo storage: card-sectors=%llu file-validated=%u file-bytes=%llu media-created=%u registered=%u\n",
  (unsigned long long)s.card_sectors,s.file_validated,(unsigned long long)s.file_bytes,s.media_created,s.media_registered);
 IOLog("Leo root: selected=%u BSD=%s major=%u minor=%u COW=%u VOLATILE=1 BASE_SD_RO=1\n",
  s.root_selected,s.bsd_name,s.major,s.minor,s.cow_ready);
 IOLog("Leo COW: cap=%u pages used=%u dirty=%u writes=%u sync=%u error=%08x\n",
  s.cow_cap,s.cow_used,s.cow_dirty,s.cow_writes,s.cow_syncs,(unsigned)s.cow_error);
 IOLog("Leo READCACHE: bytes=%u charge-pages=%u hits=%llu misses=%llu base-reads=%llu\n",s.cache_bytes,s.cache_charge_pages,(unsigned long long)s.cache_hits,(unsigned long long)s.cache_misses,(unsigned long long)s.cache_base_reads);
}
extern "C" void ios7leo_root_file_selected(const char *name,unsigned major,unsigned minor)
{
 if(!statusLock)return;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 if(rootStatus.media_created&&name&&major){
  rootStatus.root_selected=1;rootStatus.media_registered=1;
  if(rootStatus.phase!=StorageFailed)rootStatus.phase=RootSelected;
  strlcpy(rootStatus.bsd_name,name,sizeof(rootStatus.bsd_name));rootStatus.major=major;rootStatus.minor=minor;
 }
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
 ios7leo_root_file_status(); /* Selection is not a mounted-filesystem claim. */
}
extern "C" int ios7leo_cow_root_writable(unsigned root_major,unsigned root_minor)
{
 if(!statusLock)return 0;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 int ready=rootStatus.root_selected&&rootStatus.cow_ready&&rootStatus.phase!=StorageFailed&&
           rootStatus.major==root_major&&rootStatus.minor==root_minor;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);return ready;
}
class IOS7LeoHFSFile : public IOBlockStorageDevice {
 OSDeclareDefaultStructors(IOS7LeoHFSFile)
 IOMedia *raw;IOBufferMemoryDescriptor *bounce;IOLock *mutex;
 fl32_work *work;fl32_extent *extents;fl32_image image;
 UInt64 rawSectors;
 bool rawOpened,initialized,faulted,cowReady;
 leo_cow cow;UInt8 *cowArena;leo_cow_entry *cowEntries;uint32_t *cowPending;
 UInt32 cowArenaBytes,cowEntryBytes,cowPendingBytes;
 bool initializeCow();void cowStatus(IOReturn,bool,bool);
 leo_read_cache *readCache;IOBlockStorageDevice *cacheDevice;UInt64 lastCacheNote;unsigned cacheNotes;
 void initializeReadCache();bool readCacheStatus();IOReturn readImage(UInt64,UInt8[512]);
 static int readImageBase(void *,uint64_t,uint8_t[512]);static int readCacheMedia(void *);
 static int readCowBase(void *,uint64_t,uint8_t [512]);
 static int read512(void *,uint64_t,uint8_t [512]);
 IOReturn readPhysical(UInt64,UInt8 [512]);
public:
 virtual bool init(OSDictionary *properties=0);
 virtual IOService *probe(IOService *,SInt32 *);
 virtual bool start(IOService *);
 virtual void free();
 bool recognized()const{return initialized&&!faulted&&image.magic==FL32_IMAGE_MAGIC;}
 bool writable()const{return cowReady&&!faulted;}
 UInt64 fileBytes()const{return image.file_bytes;}
 virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *);
 virtual IOReturn doEjectMedia(){return kIOReturnUnsupported;}
 virtual IOReturn doFormatMedia(UInt64){return kIOReturnNotWritable;}
 virtual UInt32 doGetFormatCapacities(UInt64 *,UInt32)const{return 0;}
 virtual IOReturn doLockUnlockMedia(bool){return kIOReturnUnsupported;}
 virtual IOReturn doSynchronizeCache();
 virtual char *getVendorString();virtual char *getProductString();
 virtual char *getRevisionString();virtual char *getAdditionalDeviceInfoString();
 virtual IOReturn reportBlockSize(UInt64 *);virtual IOReturn reportMaxValidBlock(UInt64 *);
 virtual IOReturn reportEjectability(bool *);virtual IOReturn reportLockability(bool *);
 virtual IOReturn reportRemovability(bool *);virtual IOReturn reportMediaState(bool *,bool *);
 virtual IOReturn reportPollRequirements(bool *,bool *);virtual IOReturn reportWriteProtection(bool *);
};
OSDefineMetaClassAndStructors(IOS7LeoHFSFile,IOBlockStorageDevice)
bool IOS7LeoHFSFile::init(OSDictionary *properties)
{
 raw=0;bounce=0;mutex=0;work=0;extents=0;rawSectors=0;rawOpened=initialized=faulted=false;bzero(&image,sizeof(image));
 cowReady=false;cowArena=0;cowEntries=0;cowPending=0;
 cowArenaBytes=cowEntryBytes=cowPendingBytes=0;bzero(&cow,sizeof(cow));
 readCache=0;cacheDevice=0;lastCacheNote=0;cacheNotes=0;
 return IOBlockStorageDevice::init(properties);
}
IOService *IOS7LeoHFSFile::probe(IOService *provider,SInt32 *)
{
 IOMedia *media=OSDynamicCast(IOMedia,provider);
 if(!media||!media->isWhole())return 0;
 IOBlockStorageDriver *driver=OSDynamicCast(IOBlockStorageDriver,media->getProvider());
 IOService *device=driver?driver->getProvider():0;
 return device&&device->metaCast("IOS7LeoSDCC2")?this:0;
}
IOReturn IOS7LeoHFSFile::readPhysical(UInt64 lba,UInt8 bytes[512])
{
 if(!rawOpened||!raw||!bounce||lba>=rawSectors||lba>UINT64_MAX/512U)return kIOReturnBadArgument;
 UInt64 actual=0;
 /* Standard sharedReader open + original synchronous IOStorage::read preserves
  * descriptor/completion lifetimes even if its lower provider completes later. */
 IOReturn result=raw->read(this,lba<<9,bounce,&actual);
 if(result==kIOReturnSuccess&&(actual!=512||bounce->readBytes(0,bytes,512)!=512))result=kIOReturnUnderrun;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 ++rootStatus.reads;if(lba==0&&result==kIOReturnSuccess)rootStatus.read0_ok=1;
 if(result!=kIOReturnSuccess&&rootStatus.io_error==kIOReturnSuccess)rootStatus.io_error=result;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
 return result;
}
int IOS7LeoHFSFile::read512(void *cookie,uint64_t lba,uint8_t out[512])
{return (int)((IOS7LeoHFSFile *)cookie)->readPhysical(lba,out);}
int IOS7LeoHFSFile::readCowBase(void *cookie,uint64_t logical,uint8_t out[512])
{
 return (int)((IOS7LeoHFSFile *)cookie)->readImage(logical,out);
}
int IOS7LeoHFSFile::readImageBase(void *cookie,uint64_t logical,uint8_t out[512])
{
 IOS7LeoHFSFile *file=(IOS7LeoHFSFile *)cookie;uint64_t physical=0;uint32_t run=0;
 if(fl32_translate(&file->image,file->extents,LEO_EXTENT_CAP,logical,&physical,&run)!=FL32_OK)return (int)kIOReturnBadArgument;
 return (int)file->readPhysical(physical,out);
}
int IOS7LeoHFSFile::readCacheMedia(void *cookie)
{
 IOS7LeoHFSFile *file=(IOS7LeoHFSFile *)cookie;
 if(!file->cacheDevice)return (int)kIOReturnNotReady;
 bool present=false,changed=false;IOReturn error=file->cacheDevice->reportMediaState(&present,&changed);
 if(error!=kIOReturnSuccess)return (int)error;
 return present&&!changed?0:(int)kIOReturnNotReady;
}
IOReturn IOS7LeoHFSFile::readImage(UInt64 logical,UInt8 out[512])
{
 if(logical>=image.image_sectors)return kIOReturnBadArgument;
 if(readCache)return (IOReturn)leo_read_cache_read(readCache,logical,out,this,readImageBase,readCacheMedia,(int)kIOReturnBadArgument);
 return (IOReturn)readImageBase(this,logical,out);
}
void IOS7LeoHFSFile::initializeReadCache()
{
 /* Optional cache is preallocated after the required COW arena, before media
  * publication. Nine rounded pages are charged against actual free headroom. */
 const UInt32 charge=(sizeof(leo_read_cache)+4095U)>>12;
 uint64_t managed=sane_size>>12,freePages=vm_page_free_count;
 uint64_t guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 if(!managed||charge>9U||charge>(managed>>3)||freePages<=guard||charge>(freePages-guard)/2U)return;
 IOBlockStorageDriver *driver=OSDynamicCast(IOBlockStorageDriver,raw->getProvider());
 IOBlockStorageDevice *device=driver?OSDynamicCast(IOBlockStorageDevice,driver->getProvider()):0;
 if(!device||!device->metaCast("IOS7LeoSDCC2"))return;
 leo_read_cache *candidate=(leo_read_cache *)IOMalloc(sizeof(*candidate));if(!candidate)return;
 leo_read_cache_init(candidate,image.image_sectors); /* Touch owned pages before the postallocation guard. */
 guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 if((uint64_t)vm_page_free_count<guard){IOFree(candidate,sizeof(*candidate));return;}
 device->retain();cacheDevice=device;readCache=candidate;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 rootStatus.cache_bytes=sizeof(*readCache);rootStatus.cache_charge_pages=charge;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
}
bool IOS7LeoHFSFile::readCacheStatus()
{
 if(!readCache)return false;
 UInt64 observed=readCache->hits+readCache->misses;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 rootStatus.cache_hits=readCache->hits;rootStatus.cache_misses=readCache->misses;rootStatus.cache_base_reads=readCache->base_reads;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
 if(observed&&observed<=64U&&!(observed&(observed-1U))&&observed>lastCacheNote&&cacheNotes<8U){lastCacheNote=observed;cacheNotes++;return true;}
 return false;
}
bool IOS7LeoHFSFile::initializeCow()
{
 uint64_t managed=sane_size>>12;UInt32 freePages=vm_page_free_count,reserved=vm_page_free_reserved,target=vm_page_free_target;
 /* Charge rounded existing map/work/bounce, request workspace and device/lock
  * bookkeeping as well as the new arena/hash/pending allocations. */
 UInt32 fixedPages=(UInt32)((sizeof(*work)+4095U)>>12)+(UInt32)((sizeof(*extents)*LEO_EXTENT_CAP+4095U)>>12)+
                   (UInt32)((sizeof(*this)+4095U)>>12)+3U;
 UInt32 cap=leo_cow_budget_pages(managed,freePages,reserved,target,fixedPages),slots=1;
 if(cap>image.image_sectors>>3)cap=(UInt32)(image.image_sectors>>3);
 IOLog("Leo COW budget: managed=%llu free=%u reserve=%u target=%u cap=%u pages\n",(unsigned long long)managed,(unsigned)freePages,(unsigned)reserved,(unsigned)target,(unsigned)cap);
 IOLog("Leo COW total charge=%llu pages fixed/work=%u\n",(unsigned long long)leo_cow_total_pages(cap,fixedPages),(unsigned)fixedPages);
 if(!cap)return false;
 while(slots<cap*2U)slots<<=1;
 cowArenaBytes=cap<<12;cowEntryBytes=(slots*sizeof(*cowEntries)+4095U)&~4095U;cowPendingBytes=(cap*sizeof(*cowPending)+4095U)&~4095U;
 cowArena=(UInt8 *)IOMalloc(cowArenaBytes);cowEntries=(leo_cow_entry *)IOMalloc(cowEntryBytes);cowPending=(uint32_t *)IOMalloc(cowPendingBytes);
 if(!cowArena||!cowEntries||!cowPending)return false;
 /* Payload is bounded/reserved before publication; I/O never allocates under
  * this device's mutex or requires root-pageout to free new payload memory. */
 uint64_t guard=(managed>>2)+(uint64_t)vm_page_free_reserved+vm_page_free_target;
 if((uint64_t)vm_page_free_count<guard)return false;
 if(leo_cow_init(&cow,image.image_sectors,cap,slots,cowArena,cowEntries,cowPending,this,readCowBase)!=LEO_COW_OK)return false;
 cowReady=true;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);cowStatusOwner=this;rootStatus.cow_ready=1;rootStatus.cow_cap=cap;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);return true;
}
void IOS7LeoHFSFile::cowStatus(IOReturn error,bool wrote,bool synced)
{
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 rootStatus.cow_used=cow.used;rootStatus.cow_dirty=cow.dirty;
 if(wrote)rootStatus.cow_writes++;if(synced)rootStatus.cow_syncs++;
 if(error!=kIOReturnSuccess)rootStatus.cow_error=error;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);
}
bool IOS7LeoHFSFile::start(IOService *provider)
{
 if(!IOBlockStorageDevice::start(provider))return false;
 raw=OSDynamicCast(IOMedia,provider);if(!raw)return false;raw->retain();
 /* A retain does not confer storage ownership. Shared readonly access is real. */
 rawOpened=raw->open(this,0,kIOStorageAccessReader);
 if(!rawOpened){phase(StorageFailed,0,kIOReturnNotOpen);ios7leo_root_file_status();return false;}
 UInt64 size=raw->getSize();
 if(raw->getPreferredBlockSize()!=512||!size||(size&511U)){phase(StorageFailed,FL32_RANGE);return false;}
 rawSectors=size>>9;
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
 rootStatus.raw_open=1;rootStatus.card_sectors=rawSectors;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);phase(RawOpened);
 mutex=IOLockAlloc();bounce=IOBufferMemoryDescriptor::withOptions(kIODirectionIn,512,4);
 work=(fl32_work *)IOMalloc(sizeof(*work));extents=(fl32_extent *)IOMalloc(sizeof(*extents)*LEO_EXTENT_CAP);
 if(!mutex||!bounce||!work||!extents){phase(StorageFailed,0,kIOReturnNoMemory);return false;}
 fl32_io io={this,read512,rawSectors};
 fl32_limits limits={131072U,128U,939524096ULL};
 phase(LocatingFile);int result=fl32_locate(&io,&limits,work,extents,LEO_EXTENT_CAP,&image);
 if(result!=FL32_OK){phase(StorageFailed,result,(IOReturn)work->backend_error);ios7leo_root_file_status();return false;}
 if(image.magic!=FL32_IMAGE_MAGIC||image.file_bytes!=limits.expected_file_bytes||
    !image.extent_count||image.extent_count>LEO_EXTENT_CAP){phase(StorageFailed,FL32_RANGE);return false;}
 initialized=true;
 if(!initializeCow()){phase(StorageFailed,0,kIOReturnNoMemory);ios7leo_root_file_status();return false;}
 initializeReadCache();
 saved=IOSimpleLockLockDisableInterrupt(statusLock);rootStatus.file_validated=1;rootStatus.file_bytes=image.file_bytes;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);phase(ValidatedHFS);
 IOLog("Leo file: validated %s bytes=%llu extents=%u HFS=%04x version=%u BASE_SD_RO=1 COW_RW=1 VOLATILE=1\n",LEO_ROOT_FILE,
  (unsigned long long)image.file_bytes,(unsigned)image.extent_count,(unsigned)image.hfs_signature,(unsigned)image.hfs_version);
 ios7leo_root_file_status();
 IOLog("Leo root-file registerService begin validated=1 COWready=%u\n",(unsigned)cowReady);
 registerService();
 IOLog("Leo root-file registerService returned (matching requested; not media/mount proof)\n");
 return true;
}
void IOS7LeoHFSFile::free()
{
 if(statusLock){IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);if(cowStatusOwner==this){rootStatus.cow_ready=0;cowStatusOwner=0;}IOSimpleLockUnlockEnableInterrupt(statusLock,saved);}
 if(cowArena)IOFree(cowArena,cowArenaBytes);if(cowEntries)IOFree(cowEntries,cowEntryBytes);if(cowPending)IOFree(cowPending,cowPendingBytes);
 if(readCache)IOFree(readCache,sizeof(*readCache));if(cacheDevice)cacheDevice->release();
 if(rawOpened&&raw)raw->close(this);
 if(raw)raw->release();if(bounce)bounce->release();if(mutex)IOLockFree(mutex);
 if(work)IOFree(work,sizeof(*work));if(extents)IOFree(extents,sizeof(*extents)*LEO_EXTENT_CAP);
 IOBlockStorageDevice::free();
}
static IOReturn cowResult(leo_cow *cow,int result)
{
 if(result==LEO_COW_OK)return kIOReturnSuccess;
 if(result==LEO_COW_NO_SPACE)return kIOReturnNoSpace;
 if(result==LEO_COW_IO)return (IOReturn)cow->backend_error;
 return kIOReturnBadArgument;
}
IOReturn IOS7LeoHFSFile::doAsyncReadWrite(IOMemoryDescriptor *buffer,UInt64 block,UInt64 count,
 IOStorageAttributes *attributes,IOStorageCompletion *completion)
{
 if(!buffer||!completion||!completion->action||!initialized||!cowReady||!count||block>=image.image_sectors||
    count>image.image_sectors-block||count>UINT64_MAX/512U||count*512U!=buffer->getLength())return kIOReturnBadArgument;
 IODirection direction=buffer->getDirection();if(direction!=kIODirectionIn&&direction!=kIODirectionOut)return kIOReturnBadArgument;
 if(attributes&&(attributes->options||attributes->reserved0032||attributes->reserved0064))return kIOReturnUnsupported;
 IOStorageCompletion saved=*completion;UInt64 completed=0;IOReturn result=kIOReturnSuccess;UInt8 bytes[512];bool wrote=false,backendFailed=false;
 IOLockLock(mutex);if(faulted){IOLockUnlock(mutex);return kIOReturnNotReady;}
 if(direction==kIODirectionOut){int cr=leo_cow_prepare_write(&cow,block,count);backendFailed=cr==LEO_COW_IO;result=cowResult(&cow,cr);}
 for(UInt64 index=0;result==kIOReturnSuccess&&index<count;++index){
  if(direction==kIODirectionIn){
   int cr=leo_cow_read_sector(&cow,block+index,bytes);backendFailed=cr==LEO_COW_IO;result=cowResult(&cow,cr);
   if(result==kIOReturnSuccess&&buffer->writeBytes((IOByteCount)completed,bytes,512)!=512)result=kIOReturnUnderrun;
  }else{
   if(buffer->readBytes((IOByteCount)completed,bytes,512)!=512)result=kIOReturnUnderrun;
   else{result=cowResult(&cow,leo_cow_write_prepared_sector(&cow,block+index,bytes));if(result==kIOReturnSuccess)wrote=true;}
  }
  if(result==kIOReturnSuccess)completed+=512;
 }
 if(backendFailed)faulted=true;
 cowStatus(result,wrote,false);bool cacheShow=readCacheStatus();bool show=wrote&&(rootStatus.cow_writes<=2U);
 UInt64 cacheHits=readCache?readCache->hits:0,cacheMisses=readCache?readCache->misses:0,cacheBaseReads=readCache?readCache->base_reads:0;
 IOLockUnlock(mutex);
 if(faulted)phase(StorageFailed,0,result);if(show||result!=kIOReturnSuccess)ios7leo_root_file_status();
 if(cacheShow)IOLog("Leo READCACHE demand: hits=%llu misses=%llu base-reads=%llu bytes=%u\n",(unsigned long long)cacheHits,(unsigned long long)cacheMisses,(unsigned long long)cacheBaseReads,(unsigned)sizeof(*readCache));
 IOStorage::complete(&saved,result,completed);return kIOReturnSuccess;
}
IOReturn IOS7LeoHFSFile::doSynchronizeCache()
{
 if(!mutex||!initialized||!cowReady)return kIOReturnNotReady;
 IOLockLock(mutex);IOReturn result=faulted?kIOReturnIOError:kIOReturnSuccess;
 cowStatus(result,false,result==kIOReturnSuccess);bool show=rootStatus.cow_syncs<=2U;
 IOLockUnlock(mutex);if(show)ios7leo_root_file_status();return result;
}
char *IOS7LeoHFSFile::getVendorString(){static char t[]="iOS lab";return t;}
char *IOS7LeoHFSFile::getProductString(){static char t[]="Leo SD HFS file";return t;}
char *IOS7LeoHFSFile::getRevisionString(){static char t[]="Volatile COW";return t;}
char *IOS7LeoHFSFile::getAdditionalDeviceInfoString(){static char t[]=LEO_ROOT_FILE;return t;}
IOReturn IOS7LeoHFSFile::reportBlockSize(UInt64 *v){if(!v)return kIOReturnBadArgument;*v=512;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportMaxValidBlock(UInt64 *v){if(!v||!initialized)return kIOReturnNotReady;*v=image.image_sectors-1;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportEjectability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportLockability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportRemovability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportMediaState(bool *present,bool *changed)
{if(!present||!changed)return kIOReturnBadArgument;IOLockLock(mutex);*present=initialized&&!faulted;*changed=false;IOLockUnlock(mutex);return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportPollRequirements(bool *required,bool *expensive)
{if(!required||!expensive)return kIOReturnBadArgument;*required=false;*expensive=false;return kIOReturnSuccess;}
IOReturn IOS7LeoHFSFile::reportWriteProtection(bool *v){if(!v)return kIOReturnBadArgument;*v=!cowReady;return kIOReturnSuccess;}
class IOS7LeoHFSDriver : public IOBlockStorageDriver {
 OSDeclareDefaultStructors(IOS7LeoHFSDriver)
public:
 virtual bool start(IOService *);
protected:
 virtual IOMedia *instantiateMediaObject(UInt64,UInt64,UInt32,char *);
 virtual IOReturn acceptNewMedia();
};
OSDefineMetaClassAndStructors(IOS7LeoHFSDriver,IOBlockStorageDriver)
bool IOS7LeoHFSDriver::start(IOService *provider)
{
 IOLog("Leo HFSDriver start enter provider=%p\n",provider);
 bool ok=IOBlockStorageDriver::start(provider);
 IOLog("Leo HFSDriver start returned ok=%u\n",(unsigned)ok);
 return ok;
}
IOMedia *IOS7LeoHFSDriver::instantiateMediaObject(UInt64 base,UInt64 bytes,UInt32 block,char *name)
{
 IOS7LeoHFSFile *file=OSDynamicCast(IOS7LeoHFSFile,getProvider());
 if(!file||!file->recognized()||base||bytes!=file->fileBytes()||block!=512){
  IOLog("Leo HFSDriver instantiate guard failed file=%p base=%llu bytes=%llu block=%u\n",file,(unsigned long long)base,(unsigned long long)bytes,(unsigned)block);return 0;
 }
 IOMedia *media=instantiateDesiredMediaObject();if(!media){IOLog("Leo HFSDriver instantiate allocation failed\n");return 0;}
 IOMediaAttributeMask attributes=0;
 if(!media->init(0,bytes,512,attributes,true,file->writable(),"Apple_HFS")||!media->setProperty(LEO_ROOT_TAG,LEO_ROOT_FILE)){
  IOLog("Leo HFSDriver instantiate init/tag failed\n");media->release();phase(StorageFailed,FL32_HFS);return 0;
 }
 if(name)media->setName(name);
 IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);rootStatus.media_created=1;
 IOSimpleLockUnlockEnableInterrupt(statusLock,saved);phase(MediaCreated);return media;
}
IOReturn IOS7LeoHFSDriver::acceptNewMedia()
{
 IOReturn result=IOBlockStorageDriver::acceptNewMedia();
 if(result==kIOReturnSuccess){IOInterruptState saved=IOSimpleLockLockDisableInterrupt(statusLock);
  rootStatus.media_registered=1;IOSimpleLockUnlockEnableInterrupt(statusLock,saved);phase(MediaRegistered);
 }else phase(StorageFailed,0,result);
 ios7leo_root_file_status();return result;
}
static const char rootPersonalities[]=
 "<array><dict><key>IOClass</key><string>IOS7LeoHFSFile</string><key>IOProviderClass</key><string>IOMedia</string>"
 "<key>IOMatchCategory</key><string>IOS7LeoRootLocator</string><key>IOPropertyMatch</key><dict><key>Whole</key><true/></dict>"
 "<key>IOProbeScore</key><integer>10000</integer></dict>"
 "<dict><key>IOClass</key><string>IOS7LeoHFSDriver</string><key>IOProviderClass</key><string>IOS7LeoHFSFile</string>"
 "<key>IOProbeScore</key><integer>10000</integer></dict></array>";
void ios7leo_prepare_root_file()
{
 statusLock=IOSimpleLockAlloc();if(!statusLock)panic("Leo root status lock allocation failed");
 bzero(&rootStatus,sizeof(rootStatus));phase(WaitingRaw);
 OSObject *decoded=OSUnserializeXML(rootPersonalities);OSArray *drivers=OSDynamicCast(OSArray,decoded);
 if(!drivers||!gIOCatalogue->addDrivers(drivers,false))panic("Leo HFS file personality registration failed");
 decoded->release();ios7leo_root_file_status();
}
#endif
