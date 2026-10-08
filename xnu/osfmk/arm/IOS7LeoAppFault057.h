/* First failed user DATA abort per PID, for up to eight PIDs plus one overflow header.
 * Observer only: never repair registers, replace exception results, or retry.
 * Called at the existing IRQ-enabled pre-doexception point. No SD/timer/thread.
 */
#ifndef IOS7_LEO_APPFAULT057_H
#define IOS7_LEO_APPFAULT057_H
#include "IOS7LeoAppFault057Shared.h"
typedef struct LeoAppFault057Slot {
 volatile UInt32 claimed,ready,next;
 uint32_t pid;
 uint32_t length[LEO_APPFAULT057_RECORDS];
 char data[LEO_APPFAULT057_RECORDS][LEO_APPFAULT057_PAYLOAD];
} LeoAppFault057Slot;
static LeoAppFault057Slot leoAppFault057[LEO_APPFAULT057_SLOTS];
typedef struct LeoAppFault057Overflow {
 volatile UInt32 claimed,ready,next;uint32_t length;
 char data[LEO_APPFAULT057_PAYLOAD];
} LeoAppFault057Overflow;
static LeoAppFault057Overflow leoAppFault057Overflow;
typedef char leo_appfault057_budget[(sizeof(leoAppFault057)+sizeof(leoAppFault057Overflow)<=40960U)?1:-1];
/* Single-CPU LEO. IRQ masking covers only the eight-slot search and claim,
 * never formatting, copyin, device I/O or an exception decision. */
static unsigned leo_appfault057_claim_pid(uint32_t pid)
{
 boolean_t enabled=ml_set_interrupts_enabled(FALSE);
 unsigned free_slot=LEO_APPFAULT057_SLOTS;
 for(unsigned i=0;i<LEO_APPFAULT057_SLOTS;i++){
  if(leoAppFault057[i].claimed){
   if(leoAppFault057[i].pid==pid){ml_set_interrupts_enabled(enabled);return UINT32_MAX;}
  }else if(free_slot==LEO_APPFAULT057_SLOTS)free_slot=i;
 }
 if(free_slot<LEO_APPFAULT057_SLOTS){
  if(OSCompareAndSwap(0U,1U,&leoAppFault057[free_slot].claimed))leoAppFault057[free_slot].pid=pid;
  else free_slot=LEO_APPFAULT057_SLOTS;
 }
 ml_set_interrupts_enabled(enabled);return free_slot;
}
static void leo_appfault057_overflow(uint32_t pid,const char *name,
 const ios7leo037_data_snapshot_t *snapshot,kern_return_t result,uint32_t bad)
{
 if(!OSCompareAndSwap(0U,1U,&leoAppFault057Overflow.claimed))return;
 const abort_information_context_t *r=&snapshot->entry;
 int n=snprintf(leoAppFault057Overflow.data,LEO_APPFAULT057_PAYLOAD,
  "APPFAULT057 FIRST_OVERFLOW PID=%u NAME=%s RESULT=%x BAD=%08x PC=%08x LR=%08x SP=%08x R0_R3=%08x,%08x,%08x,%08x PSR=%08x FSR=%08x FAR=%08x further-full-context-limited\n",
  (unsigned)pid,name,(unsigned)result,(unsigned)bad,(unsigned)r->pc,
  (unsigned)r->lr,(unsigned)r->sp,(unsigned)r->r[0],(unsigned)r->r[1],
  (unsigned)r->r[2],(unsigned)r->r[3],(unsigned)r->cpsr,(unsigned)r->fsr,(unsigned)r->far);
 if(n<=0||(unsigned)n>=LEO_APPFAULT057_PAYLOAD){
  static const char error[]="APPFAULT057 OVERFLOW_FORMAT_ERROR\n";
  memcpy(leoAppFault057Overflow.data,error,sizeof(error)-1U);n=sizeof(error)-1U;
 }
 leoAppFault057Overflow.length=(uint32_t)n;OSMemoryBarrier();leoAppFault057Overflow.ready=1U;
}
static int leo_appfault057_range(uint32_t start,uint32_t bytes)
{
 return bytes && start>=0x1000U && start<=0x40000000U-bytes;
}
static void leo_appfault057_length(LeoAppFault057Slot *slot,unsigned stage,int n)
{
 if(stage>=LEO_APPFAULT057_RECORDS)return;
 if(n<=0 || (unsigned)n>=LEO_APPFAULT057_PAYLOAD){
  static const char error[]="APPFAULT057 FORMAT_ERROR incomplete-record\n";
  memcpy(slot->data[stage],error,sizeof(error)-1U);slot->length[stage]=sizeof(error)-1U;
 }else slot->length[stage]=(uint32_t)n;
}
static void leo_appfault057_hex(const uint8_t *bytes,unsigned count,char *out)
{
 static const char digits[]="0123456789abcdef";
 for(unsigned i=0;i<count;i++){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15U];}
 out[2*count]=0;
}
static void leo_appfault057_context(LeoAppFault057Slot *slot,unsigned role,
 unsigned stage,const char *phase,const abort_information_context_t *r)
{
 int n=snprintf(slot->data[stage],LEO_APPFAULT057_PAYLOAD,
  "APPFAULT057 SLOT=%u PID=%u %s R=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x SP=%08x LR=%08x PC=%08x PSR=%08x FSR=%08x FAR=%08x\n",
  role,slot->pid,phase,(unsigned)r->r[0],(unsigned)r->r[1],(unsigned)r->r[2],
  (unsigned)r->r[3],(unsigned)r->r[4],(unsigned)r->r[5],(unsigned)r->r[6],
  (unsigned)r->r[7],(unsigned)r->r[8],(unsigned)r->r[9],(unsigned)r->r[10],
  (unsigned)r->r[11],(unsigned)r->r[12],(unsigned)r->sp,(unsigned)r->lr,
  (unsigned)r->pc,(unsigned)r->cpsr,(unsigned)r->fsr,(unsigned)r->far);
 leo_appfault057_length(slot,stage,n);
}
static int leo_appfault057_copy(uint32_t address,void *buffer,uint32_t bytes,int allowed)
{
 if(!allowed)return -1;
 if(!leo_appfault057_range(address,bytes))return -2;
 return copyin((user_addr_t)address,buffer,bytes);
}
static void leo_appfault057_memory(LeoAppFault057Slot *slot,unsigned role,
 unsigned stage,const char *label,uint32_t start,unsigned count,int allowed)
{
 uint8_t bytes[164];char hex[329];int error,n;
 if(count>sizeof(bytes))error=-2;
 else error=leo_appfault057_copy(start,bytes,count,allowed);
 if(error){
  n=snprintf(slot->data[stage],LEO_APPFAULT057_PAYLOAD,
   "APPFAULT057 SLOT=%u PID=%u %s START=%08x BYTES=%u VALID=0 ERR=%d no-data\n",
   role,slot->pid,label,(unsigned)start,count,error);
 }else{
  leo_appfault057_hex(bytes,count,hex);
  n=snprintf(slot->data[stage],LEO_APPFAULT057_PAYLOAD,
   "APPFAULT057 SLOT=%u PID=%u %s START=%08x BYTES=%u VALID=1 DATA=%s\n",
   role,slot->pid,label,(unsigned)start,count,hex);
 }
 leo_appfault057_length(slot,stage,n);
}
__attribute__((noinline)) static void ios7leo057_failed_app_data(
 thread_t owner,const ios7leo037_data_snapshot_t *snapshot,
 kern_return_t result,uint32_t bad)
{
 unsigned role,i,depth=0;int allowed,pid,n,frame_error=0;uint32_t frame,sp,ceiling;
 uint32_t links[20],link[2],info[3];char hex[161];const char *name;
 LeoAppFault057Slot *slot;
 if(!snapshot||!snapshot->valid||!owner||!owner->task||!owner->task->bsd_info||
    result==KERN_SUCCESS||result==KERN_ABORTED||(snapshot->entry.cpsr&0x1fU)!=0x10U)return;
 name=proc_name_address(owner->task->bsd_info);if(!name)name="?";
 pid=proc_pid(owner->task->bsd_info);if(pid<=0)return;
 role=leo_appfault057_claim_pid((uint32_t)pid);
 if(role==UINT32_MAX)return;
 if(role>=LEO_APPFAULT057_SLOTS){leo_appfault057_overflow((uint32_t)pid,name,snapshot,result,bad);return;}
 slot=&leoAppFault057[role];
 /* Wrong owner/context still gets immutable registers, but no user reads. */
 allowed=(owner==current_thread() && owner->machine.uss &&
  snapshot->context_address==snapshot->owner_uss_entry &&
  snapshot->owner_uss_after==snapshot->owner_uss_entry &&
  snapshot->c_thread_after==snapshot->c_thread_entry);
 n=snprintf(slot->data[0],LEO_APPFAULT057_PAYLOAD,
  "APPFAULT057 SLOT=%u PID=%u NAME=%s TID=%llx RESULT=%x BAD=%08x CTX=%08x OUSS=%08x/%08x CTH=%08x/%08x CPTH=%08x/%08x DIFF=%x COPY_ALLOWED=%u DYLDINFO=%llx/%llx\n",
  role,slot->pid,name,(unsigned long long)owner->thread_id,(unsigned)result,(unsigned)bad,
  (unsigned)snapshot->context_address,(unsigned)snapshot->owner_uss_entry,
  (unsigned)snapshot->owner_uss_after,(unsigned)snapshot->c_thread_entry,
  (unsigned)snapshot->c_thread_after,(unsigned)snapshot->cp15_thread_entry,
  (unsigned)snapshot->cp15_thread_after,
  (unsigned)ios7leo037_context_diff(&snapshot->entry,&snapshot->after_vm),
  (unsigned)allowed,(unsigned long long)owner->task->all_image_info_addr,
  (unsigned long long)owner->task->all_image_info_size);
 leo_appfault057_length(slot,0,n);
 leo_appfault057_context(slot,role,1,"ENTRY",&snapshot->entry);
 leo_appfault057_context(slot,role,2,"AFTER_VM",&snapshot->after_vm);
 {
  uint32_t pc=snapshot->entry.pc&~1U,lr=snapshot->entry.lr&~1U;
  leo_appfault057_memory(slot,role,3,"PC_BYTES",pc>=4U?pc-4U:0U,16U,allowed);
  leo_appfault057_memory(slot,role,4,"LR_BYTES",lr>=4U?lr-4U:0U,16U,allowed);
 }
 {
  uint64_t address=owner->task->all_image_info_addr,size=owner->task->all_image_info_size;
  unsigned count=size<164U?(unsigned)size:164U;
  int info_allowed=allowed && address<=UINT32_MAX && size>=12U && size<=4096U;
  leo_appfault057_memory(slot,role,5,"DYLD_INFO",(uint32_t)address,count,info_allowed);
  int error=leo_appfault057_copy((uint32_t)address,info,sizeof(info),info_allowed);
  if(!error && info[0]>0U && info[0]<=32U && info[1]>0U && info[1]<=4096U){
   unsigned images=info[1]<8U?info[1]:8U;
   leo_appfault057_memory(slot,role,10,"IMAGE_ARRAY",info[2],images*12U,allowed);
  }else{
   n=snprintf(slot->data[10],LEO_APPFAULT057_PAYLOAD,
    "APPFAULT057 SLOT=%u PID=%u IMAGE_ARRAY VALID=0 PREFIX_ERR=%d invalid-version-count-or-copy\n",role,slot->pid,error);
   leo_appfault057_length(slot,10,n);
  }
 }
 sp=snapshot->entry.sp;
 for(i=0;i<3U;i++){
  static const char *labels[3]={"STACK0","STACK1","STACK2"};
  int stack_allowed=allowed && !(sp&3U) && leo_appfault057_range(sp,384U);
  leo_appfault057_memory(slot,role,6U+i,labels[i],sp+i*128U,128U,stack_allowed);
 }
 frame=snapshot->entry.r[7];ceiling=sp<=0x40000000U-65536U?sp+65536U:0U;
 if(!allowed || (sp&3U)||!leo_appfault057_range(sp,65536U))frame_error=-1;
 else for(depth=0;depth<10U;depth++){
  if((frame&3U)||frame<sp||frame>ceiling-8U){frame_error=-2;break;}
  frame_error=leo_appfault057_copy(frame,link,sizeof(link),allowed);if(frame_error)break;
  links[depth*2]=frame;links[depth*2+1]=link[1];
  if(link[0]<=frame){depth++;frame_error=-3;break;}
  frame=link[0];
 }
 leo_appfault057_hex((const uint8_t *)links,depth*8U,hex);
 n=snprintf(slot->data[9],LEO_APPFAULT057_PAYLOAD,
  "APPFAULT057 SLOT=%u PID=%u FRAMES COUNT=%u STOP_ERR=%d CAP=10 FP_RETURN_LE32=%s partial-copyin-not-unwinder\n",
  role,slot->pid,depth,frame_error,hex);
 leo_appfault057_length(slot,9,n);
 /* Lengths and every record become immutable before release publication. */
 OSMemoryBarrier();slot->ready=1U;
}
uint32_t ios7leo_appfault057_take(uint8_t *payload,uint32_t capacity)
{
 if(!payload||!capacity)return 0U;
 for(unsigned role=0;role<LEO_APPFAULT057_SLOTS;role++){
  LeoAppFault057Slot *slot=&leoAppFault057[role];UInt32 next;uint32_t length;
  if(slot->ready!=1U){continue;}
  OSMemoryBarrier();
  do{
   next=slot->next;if(next>=LEO_APPFAULT057_RECORDS)break;
   length=slot->length[next];if(!length||length>=LEO_APPFAULT057_PAYLOAD||capacity<length)return 0U;
  }while(!OSCompareAndSwap(next,next+1U,&slot->next));
  if(next>=LEO_APPFAULT057_RECORDS)continue;
  memcpy(payload,slot->data[next],length);return length;
 }
 if(leoAppFault057Overflow.ready==1U){
  OSMemoryBarrier();uint32_t length=leoAppFault057Overflow.length;
  if(length&&length<LEO_APPFAULT057_PAYLOAD&&capacity>=length&&OSCompareAndSwap(0U,1U,&leoAppFault057Overflow.next)){
   memcpy(payload,leoAppFault057Overflow.data,length);return length;
  }
 }
 return 0U;
}
#endif
