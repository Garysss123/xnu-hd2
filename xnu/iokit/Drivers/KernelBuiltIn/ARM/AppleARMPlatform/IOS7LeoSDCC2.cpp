#if defined(BOARD_CONFIG_QSD8250_LEO)
/* Genuine Leo SDCC2: ordinary raw media read-only; private owned-log writes only.
 * No DMA or SDCC IRQ registration. */
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/debug.h>
#include "leo_sdcc.h"
#include "IOS7LeoSDLog.h"
#include "IOS7LabSDProtocol.h"
#include "IOS7LabStoragePersonalities.h"
extern "C" {
#include <mach/mach_time.h>
}
class IOS7LeoSDCC2;
struct ios7leo_sd_log_capability {
 IOS7LeoSDCC2 *owner;
 struct ios7leo_sd_log_extent extents[IOS7LEO_SD_LOG_MAX_EXTENTS];
 uint32_t extent_count,next_sector,first_verified;
 uint8_t nonce[16];
};
/* Single owned log for boot lifetime; compare opaque handles before dereference. */
static ios7leo_sd_log_capability *boundLogCapability;
class IOS7LeoSDCC2 : public IOBlockStorageDevice {
 OSDeclareDefaultStructors(IOS7LeoSDCC2)
 IOMemoryDescriptor *mmio[4];IOMemoryMap *maps[4];volatile UInt32 *regs[4];
 IOLock *mutex;struct leo_sd_card card;bool initialized;
 static uint32_t readMMIO(void *,unsigned,unsigned);
 static void writeMMIO(void *,unsigned,unsigned,uint32_t);
 static uint64_t nowNS(void *);
 static int delayUS(void *,uint32_t);
 static IOReturn convert(int);
public:
 virtual bool init(OSDictionary *properties=0);
 virtual bool start(IOService *provider);
 void failurePanic(const char *reason);
 virtual void free();
 virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *);
 virtual IOReturn doEjectMedia(){return kIOReturnUnsupported;}
 virtual IOReturn doFormatMedia(UInt64){return kIOReturnUnsupported;}
 virtual UInt32 doGetFormatCapacities(UInt64 *,UInt32)const{return 0;}
 virtual IOReturn doLockUnlockMedia(bool){return kIOReturnUnsupported;}
 virtual IOReturn doSynchronizeCache(){return kIOReturnUnsupported;}
 virtual char *getVendorString();virtual char *getProductString();
 virtual char *getRevisionString();virtual char *getAdditionalDeviceInfoString();
 virtual IOReturn reportBlockSize(UInt64 *);
 virtual IOReturn reportMaxValidBlock(UInt64 *);
 virtual IOReturn reportEjectability(bool *);
 virtual IOReturn reportLockability(bool *);
 virtual IOReturn reportRemovability(bool *);
 virtual IOReturn reportMediaState(bool *,bool *);
 virtual IOReturn reportPollRequirements(bool *,bool *);
 virtual IOReturn reportWriteProtection(bool *);
 int describeLog(struct ios7leo_sd_log_info *);
 int bindLog(const struct ios7leo_sd_log_extent *,uint32_t,uint32_t,const uint8_t [16],ios7leo_sd_log_capability **);
 int writeLog(ios7leo_sd_log_capability *,uint32_t,const uint8_t [512]);
};
OSDefineMetaClassAndStructors(IOS7LeoSDCC2,IOBlockStorageDevice)
uint32_t IOS7LeoSDCC2::readMMIO(void *opaque,unsigned region,unsigned offset)
{
 IOS7LeoSDCC2 *s=(IOS7LeoSDCC2 *)opaque;
 __asm__ __volatile__("dsb sy":::"memory");UInt32 value=s->regs[region][offset/4];__asm__ __volatile__("dsb sy":::"memory");return value;
}
void IOS7LeoSDCC2::writeMMIO(void *opaque,unsigned region,unsigned offset,uint32_t value)
{
 IOS7LeoSDCC2 *s=(IOS7LeoSDCC2 *)opaque;
 __asm__ __volatile__("dsb sy":::"memory");s->regs[region][offset/4]=value;__asm__ __volatile__("dsb sy":::"memory");
}
uint64_t IOS7LeoSDCC2::nowNS(void *){return mach_absolute_time();}
int IOS7LeoSDCC2::delayUS(void *,uint32_t us)
{
 UInt64 start=mach_absolute_time(),limit=(UInt64)us*1000U;
 /* Uses actual GPT->ns clock, not a guessed CPU frequency/IODelay loop. */
 for(unsigned n=0;n<5000000U;++n){if(mach_absolute_time()-start>=limit)return 1;}
 return 0;
}
IOReturn IOS7LeoSDCC2::convert(int result)
{
 if(result==LEO_SD_OK)return kIOReturnSuccess;
 if(result==LEO_SD_TIMEOUT||result==LEO_SD_RPC_UNCERTAIN)return kIOReturnTimeout;
 if(result==LEO_SD_ARGUMENT)return kIOReturnBadArgument;
 if(result==LEO_SD_FAULTED||result==LEO_SD_RPC_BUSY)return kIOReturnNotReady;
 if(result==LEO_SD_SHORT)return kIOReturnUnderrun;
 return kIOReturnIOError;
}
bool IOS7LeoSDCC2::init(OSDictionary *properties)
{
 for(unsigned i=0;i<4;++i){mmio[i]=0;maps[i]=0;regs[i]=0;}
 mutex=0;initialized=false;
 struct leo_sd_io io={this,readMMIO,writeMMIO,nowNS,delayUS,3000000U};leo_sd_init(&card,&io);
 return IOBlockStorageDevice::init(properties);
}
bool IOS7LeoSDCC2::start(IOService *provider)
{
 if(!IOBlockStorageDevice::start(provider))return false;
 const UInt32 physical[4]={0xa0400000U,0x00100000U,0xac100000U,0xa8600000U};
 card.stage=100;mutex=IOLockAlloc();if(!mutex)return false;
 for(unsigned i=0;i<4;++i){
  card.stage=101+i;
  mmio[i]=IOMemoryDescriptor::withPhysicalAddress(physical[i],4096,kIODirectionInOut);
  if(!mmio[i])return false;
  maps[i]=mmio[i]->map(kIOMapInhibitCache);if(!maps[i])return false;
  regs[i]=(volatile UInt32 *)maps[i]->getVirtualAddress();if(!regs[i])return false;
 }
 IOLog("Leo SDCC2: init read-only 144kHz one-bit begin\n");
 IOLockLock(mutex);
 int result=leo_sd_start(&card);UInt8 sector[512];
 if(result==LEO_SD_OK)result=leo_sd_read_sector(&card,0,sector);
 if(result==LEO_SD_OK)result=leo_sd_transfer_clock(&card,sector);
 IOLockUnlock(mutex);
 IOLog("Leo SDCC2: requested-clock=%uHz selector=%u CSD-max=%uHz slow-read0=%u fast-read0=%u result=%d\n",
  (unsigned)card.requested_clock_hz,(unsigned)card.requested_selector,(unsigned)card.csd_transfer_hz,
  (unsigned)card.slow_read0_ok,(unsigned)card.fast_read0_verified,result);
 if(result!=LEO_SD_OK){
  IOLog("Leo SDCC2: error=%d stage=%u command=%u status=%08x rpc=%u uncertain=%u\n",
   result,card.stage,card.command,card.status,card.rpc_status,(unsigned)card.rpc_uncertain);
  panic("Leo SDCC2 init/read0 error=%d stage=%u cmd=%u status=%08x RPC=%u arg=%08x:%08x reply=%08x uncertain=%u",
   result,card.stage,card.command,card.status,card.last_rpc_command,card.last_rpc_arg1,card.last_rpc_arg2,card.rpc_status,(unsigned)card.rpc_uncertain);
  return false;
 }
 UInt32 checksum=2166136261U;
 for(unsigned i=0;i<512;++i){checksum^=sector[i];checksum*=16777619U;}
 IOLog("Leo SDCC2: CID=%08x:%08x:%08x:%08x CSD=%08x:%08x:%08x:%08x\n",
  card.cid[0],card.cid[1],card.cid[2],card.cid[3],card.csd[0],card.csd[1],card.csd[2],card.csd[3]);
 IOLog("Leo SDCC2: sectors=%llu SDHC=%u CSD-WP=%u sector0-FNV=%08x MBRsig=%02x%02x readonly=1\n",
  (unsigned long long)card.sectors,(unsigned)card.high_capacity,(unsigned)card.csd_write_protected,
  checksum,(unsigned)sector[510],(unsigned)sector[511]);
 initialized=true;return true;
}
void IOS7LeoSDCC2::failurePanic(const char *reason)
{panic("Leo SDCC2 mandatory startup %s stage=%u cmd=%u status=%08x RPC=%u arg=%08x:%08x reply=%08x uncertain=%u",
 reason,card.stage,card.command,card.status,card.last_rpc_command,card.last_rpc_arg1,card.last_rpc_arg2,card.rpc_status,(unsigned)card.rpc_uncertain);}
void IOS7LeoSDCC2::free()
{
 /* No mailbox/controller reset after uncertain completion; only owned mappings freed. */
 for(unsigned i=0;i<4;++i){if(maps[i])maps[i]->release();if(mmio[i])mmio[i]->release();}
 if(mutex)IOLockFree(mutex);IOBlockStorageDevice::free();
}
IOReturn IOS7LeoSDCC2::doAsyncReadWrite(IOMemoryDescriptor *buffer,UInt64 block,UInt64 count,
 IOStorageAttributes *attributes,IOStorageCompletion *completion)
{
 UInt64 bytes=0;
 if(!buffer||!completion||!completion->action||!initialized||
  !ios7lab_sd_request_range(card.sectors,block,count,buffer->getLength(),&bytes))return kIOReturnBadArgument;
 if(buffer->getDirection()!=kIODirectionIn)return kIOReturnNotWritable;
 if(attributes&&(attributes->options||attributes->reserved0032||attributes->reserved0064))return kIOReturnUnsupported;
 IOStorageCompletion saved=*completion;UInt64 completed=0;IOReturn result=kIOReturnSuccess;UInt8 data[512];
 IOLockLock(mutex);
 if(card.faulted){IOLockUnlock(mutex);return kIOReturnNotReady;}
 for(UInt64 i=0;i<count;++i){
  result=convert(leo_sd_read_sector(&card,block+i,data));if(result!=kIOReturnSuccess)break;
  if(buffer->writeBytes((IOByteCount)completed,data,512)!=512){result=kIOReturnUnderrun;break;}
  completed+=512;
 }
 IOLockUnlock(mutex);IOStorage::complete(&saved,result,completed);return kIOReturnSuccess;
}
char *IOS7LeoSDCC2::getVendorString(){static char t[]="HTC";return t;}
char *IOS7LeoSDCC2::getProductString(){static char t[]="Leo SDCC2 SD";return t;}
char *IOS7LeoSDCC2::getRevisionString(){static char t[]="PIO RO";return t;}
char *IOS7LeoSDCC2::getAdditionalDeviceInfoString(){static char t[]="QSD8250 GPIO62-67 GP6";return t;}
IOReturn IOS7LeoSDCC2::reportBlockSize(UInt64 *v){if(!v)return kIOReturnBadArgument;*v=512;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportMaxValidBlock(UInt64 *v){if(!v||!initialized)return kIOReturnNotReady;*v=card.sectors-1;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportEjectability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportLockability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportRemovability(bool *v){if(!v)return kIOReturnBadArgument;*v=true;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportMediaState(bool *present,bool *changed)
{
 if(!present||!changed)return kIOReturnBadArgument;
 IOLockLock(mutex);*present=initialized&&!card.faulted;*changed=false;IOLockUnlock(mutex);
 /* Presence derives from successful protocol, not a claimed GPIO153 measurement.
  * Runtime transport errors latch unavailable; insertion/hotplug is unsupported. */
 return kIOReturnSuccess;
}
IOReturn IOS7LeoSDCC2::reportPollRequirements(bool *required,bool *expensive)
{if(!required||!expensive)return kIOReturnBadArgument;*required=false;*expensive=false;return kIOReturnSuccess;}
IOReturn IOS7LeoSDCC2::reportWriteProtection(bool *v){if(!v)return kIOReturnBadArgument;*v=true;return kIOReturnSuccess;}
int IOS7LeoSDCC2::describeLog(struct ios7leo_sd_log_info *info)
{
 if(!info||!mutex)return LEO_SD_ARGUMENT;
 IOLockLock(mutex);
 info->card_sectors=card.sectors;for(unsigned i=0;i<4;++i)info->cid[i]=card.cid[i];
 info->csd_write_protected=(uint32_t)card.csd_write_protected;info->initialized=initialized;
 info->faulted=(uint32_t)card.faulted;info->stage=card.stage;info->command=card.command;info->status=card.status;
 info->write_attempts=card.write_attempts;info->write_words=card.write_words;info->write_completed=card.write_completed;info->write_result=card.write_result;
 info->first_write_verified=0;info->next_file_sector=0;
 if(boundLogCapability&&boundLogCapability->owner==this){info->first_write_verified=boundLogCapability->first_verified;info->next_file_sector=boundLogCapability->next_sector;}
 IOLockUnlock(mutex);return LEO_SD_OK;
}
int IOS7LeoSDCC2::bindLog(const struct ios7leo_sd_log_extent *extents,uint32_t count,
 uint32_t first,const uint8_t nonce[16],ios7leo_sd_log_capability **out)
{
 if(!out){return LEO_SD_ARGUMENT;}
 *out=0;
 if(!mutex||!extents||!nonce||!count||count>IOS7LEO_SD_LOG_MAX_EXTENTS||!first||first>IOS7LEO_SD_LOG_FILE_SECTORS)return LEO_SD_ARGUMENT;
 unsigned nonzero=0;for(unsigned i=0;i<16;++i)nonzero|=nonce[i];if(!nonzero)return LEO_SD_ARGUMENT;
 /* Allocate before taking the hardware mutex: no allocation during root I/O. */
 ios7leo_sd_log_capability *cap=(ios7leo_sd_log_capability *)IOMalloc(sizeof(*cap));
 if(!cap)return LEO_SD_NO_MEMORY;
 bzero(cap,sizeof(*cap));int result=LEO_SD_OK;uint64_t cursor=0;
 IOLockLock(mutex);
 if(!initialized||card.faulted||card.rpc_uncertain)result=LEO_SD_FAULTED;
 else if(card.csd_write_protected)result=LEO_SD_WRITE_PROTECTED;
 else if(boundLogCapability)result=LEO_SD_ARGUMENT;
 for(unsigned i=0;result==LEO_SD_OK&&i<count;++i){
  const struct ios7leo_sd_log_extent *e=extents+i;
  if(!e->sectors||e->file_sector!=cursor||cursor>IOS7LEO_SD_LOG_FILE_SECTORS||e->sectors>IOS7LEO_SD_LOG_FILE_SECTORS-cursor||
     !e->card_sector||e->card_sector>=card.sectors||e->sectors>card.sectors-e->card_sector){result=LEO_SD_ARGUMENT;break;}
  for(unsigned j=0;j<i;++j){const struct ios7leo_sd_log_extent *p=extents+j;
   if(e->card_sector<p->card_sector+p->sectors&&p->card_sector<e->card_sector+e->sectors){result=LEO_SD_ARGUMENT;break;}}
  cap->extents[i]=*e;cursor+=e->sectors;
 }
 if(result==LEO_SD_OK&&cursor!=IOS7LEO_SD_LOG_FILE_SECTORS)result=LEO_SD_ARGUMENT;
 if(result==LEO_SD_OK){
  cap->owner=this;cap->extent_count=count;cap->next_sector=first;
  for(unsigned i=0;i<16;++i)cap->nonce[i]=nonce[i];
  retain(); /* A bounded boot-lifetime capability owns this real service. */
  boundLogCapability=cap;*out=cap;
 }
 IOLockUnlock(mutex);if(result!=LEO_SD_OK)IOFree(cap,sizeof(*cap));return result;
}
int IOS7LeoSDCC2::writeLog(ios7leo_sd_log_capability *cap,uint32_t logical,const uint8_t bytes[512])
{
 if(!mutex||!cap||!bytes||!logical||logical>=IOS7LEO_SD_LOG_FILE_SECTORS||cap!=boundLogCapability)return LEO_SD_ARGUMENT;
 uint64_t physical=0;int result=LEO_SD_ARGUMENT;uint8_t verified[512];
 IOLockLock(mutex);
 if(cap!=boundLogCapability||cap->owner!=this||logical!=cap->next_sector){IOLockUnlock(mutex);return LEO_SD_ARGUMENT;}
 if(!initialized||card.faulted||card.rpc_uncertain){IOLockUnlock(mutex);return LEO_SD_FAULTED;}
 for(unsigned i=0;i<cap->extent_count;++i){const struct ios7leo_sd_log_extent *e=cap->extents+i;
  if(logical>=e->file_sector&&(uint64_t)logical-e->file_sector<e->sectors){physical=e->card_sector+((uint64_t)logical-e->file_sector);result=LEO_SD_OK;break;}}
 if(result==LEO_SD_OK)result=leo_sd_write_sector(&card,physical,bytes);
 if(result==LEO_SD_OK&&!cap->first_verified){
  result=leo_sd_read_sector(&card,physical,verified);
  if(result==LEO_SD_OK)for(unsigned i=0;i<512;++i)if(verified[i]!=bytes[i]){result=LEO_SD_PROTOCOL;card.faulted=1;break;}
  if(result==LEO_SD_OK)cap->first_verified=1;
 }
 card.write_result=result;
 if(result==LEO_SD_OK)++cap->next_sector;
 IOLockUnlock(mutex);return result;
}
extern "C" int ios7leo_sd_log_describe(void *service,struct ios7leo_sd_log_info *info)
{
 IOS7LeoSDCC2 *device=service?OSDynamicCast(IOS7LeoSDCC2,(OSObject *)service):0;
 return device?device->describeLog(info):LEO_SD_ARGUMENT;
}
extern "C" int ios7leo_sd_log_bind(void *service,const struct ios7leo_sd_log_extent *extents,
 uint32_t count,uint32_t first,const uint8_t nonce[16],ios7leo_sd_log_capability **out)
{
 if(out)*out=0;
 IOS7LeoSDCC2 *device=service?OSDynamicCast(IOS7LeoSDCC2,(OSObject *)service):0;
 return device?device->bindLog(extents,count,first,nonce,out):LEO_SD_ARGUMENT;
}
extern "C" int ios7leo_sd_log_write(ios7leo_sd_log_capability *cap,uint32_t logical,const uint8_t bytes[512])
{
 if(!cap||cap!=boundLogCapability)return LEO_SD_ARGUMENT;
 return cap->owner->writeLog(cap,logical,bytes);
}
void ios7leo_register_sd(IOService *platform)
{
 IOS7LeoSDCC2 *device=new IOS7LeoSDCC2;if(!device){panic("Leo SDCC2 mandatory object allocation failed");return;}
 bool attached=false,success=false;
 if(device->init())attached=device->attach(platform);
 if(attached&&device->start(platform)){
  OSObject *decoded=OSUnserializeXML(ios7labStoragePersonalities);
  OSArray *drivers=OSDynamicCast(OSArray,decoded);
  if(drivers&&gIOCatalogue->addDrivers(drivers,false)){device->registerService();success=true;}
  else IOLog("Leo SDCC2: original storage catalogue registration failed\n");
  if(decoded)decoded->release();
 }
 if(!success)device->failurePanic("object init/attach/map/start/catalogue failed");
 if(attached&&!success)device->detach(platform);
 device->release();
}
#endif
