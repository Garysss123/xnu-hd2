#ifndef IOS7LEO_PREFERENCES_RPC060_H
#define IOS7LEO_PREFERENCES_RPC060_H
#include "IOS7LeoPreferencesRPC060Shared.h"
/* LEO is single-CPU. Fixed scalar updates only under a short IRQ mask.
 * Scope/clock reads occur before masking. No user read, port lookup, allocation,
 * formatting or storage. Header comes from the normal successful kmsg_get. */
static LeoPrefsRPC060 leoPrefsRPC060;
static volatile uint32_t leoPrefsRPC060Stopped;
typedef char leo_rpc060_budget[(sizeof(leoPrefsRPC060)<=192U)?1:-1];
static int leo_rpc060_identity(thread_t t,uint32_t *pid,uint32_t *version)
{
 if(leoPrefsRPC060Stopped||!t||!(thread_get_tag_internal(t)&THREAD_TAG_MAINTHREAD)||!t->task||!t->task->bsd_info)return 0;
 const char *name=proc_name_address(t->task->bsd_info);
 int n=proc_pid(t->task->bsd_info);
 if(n<=0||!name||strncmp(name,"Preferences",12U))return 0;
 *pid=(uint32_t)n;*version=(uint32_t)proc_pidversion(t->task->bsd_info);return 1;
}
static uint32_t leo_rpc060_begin(struct mach_msg_overwrite_trap_args *a)
{
 thread_t t=current_thread();uint32_t pid,version;
 if(!leo_rpc060_identity(t,&pid,&version))return 0U;
 uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);
 if(leoPrefsRPC060.known&&(leoPrefsRPC060.pid!=pid||leoPrefsRPC060.pidversion!=version||leoPrefsRPC060.tid!=t->thread_id)){
  ml_set_interrupts_enabled(irq);return 0U;
 }
 uint32_t seq=leoPrefsRPC060.call;
 if(seq==UINT32_MAX){ml_set_interrupts_enabled(irq);return 0U;}
 bzero(&leoPrefsRPC060,sizeof(leoPrefsRPC060));
 leoPrefsRPC060.known=1U;leoPrefsRPC060.pid=pid;leoPrefsRPC060.pidversion=version;
 leoPrefsRPC060.tid=t->thread_id;leoPrefsRPC060.call=++seq;leoPrefsRPC060.stage=LEO_RPC060_ENTRY;
 leoPrefsRPC060.entry_absolute=leoPrefsRPC060.last_absolute=now;
 leoPrefsRPC060.message_address=a->msg;
 leoPrefsRPC060.receive_address=(a->option&MACH_RCV_OVERWRITE)?a->rcv_msg:(a->rcv_msg?a->rcv_msg:a->msg);
 leoPrefsRPC060.option=a->option;leoPrefsRPC060.send_size=a->send_size;
 leoPrefsRPC060.receive_limit=a->rcv_size;leoPrefsRPC060.receive_name=a->rcv_name;leoPrefsRPC060.timeout=a->timeout;
 OSMemoryBarrier();ml_set_interrupts_enabled(irq);return seq;
}
static void leo_rpc060_note(uint32_t seq,uint32_t stage,uint32_t result,const mach_msg_header_t *header)
{
 if(!seq||leoPrefsRPC060Stopped)return;
 thread_t t=current_thread();uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);
 if(leoPrefsRPC060.call==seq&&leoPrefsRPC060.tid==t->thread_id){
  leoPrefsRPC060.stage=stage;leoPrefsRPC060.result=result;leoPrefsRPC060.last_absolute=now;
  if(header){
   leoPrefsRPC060.header_valid=1;leoPrefsRPC060.message_id=header->msgh_id;
   leoPrefsRPC060.header_size=header->msgh_size;leoPrefsRPC060.bits=header->msgh_bits;
   leoPrefsRPC060.remote_name=(uint32_t)(uintptr_t)header->msgh_remote_port;
   leoPrefsRPC060.local_name=(uint32_t)(uintptr_t)header->msgh_local_port;
  }
  if(stage==LEO_RPC060_WAIT)leoPrefsRPC060.active_receive=1;
  if(stage==LEO_RPC060_DONE)leoPrefsRPC060.active_receive=0;
  OSMemoryBarrier();
 }
 ml_set_interrupts_enabled(irq);
}
static uint32_t leo_rpc060_receive_begin(thread_t t,mach_msg_return_t mr,const mach_msg_header_t *header)
{
 uint32_t pid,version;if(!leo_rpc060_identity(t,&pid,&version))return 0;
 uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);uint32_t seq=0;
 if(leoPrefsRPC060.known&&leoPrefsRPC060.active_receive&&leoPrefsRPC060.pid==pid&&leoPrefsRPC060.pidversion==version&&leoPrefsRPC060.tid==t->thread_id&&
    leoPrefsRPC060.option==t->ith_option&&leoPrefsRPC060.receive_address==t->ith_msg_addr&&t->ith_continuation==thread_syscall_return){
  seq=leoPrefsRPC060.call;leoPrefsRPC060.stage=LEO_RPC060_RECEIVE;
  leoPrefsRPC060.receive_state=(uint32_t)mr;leoPrefsRPC060.last_absolute=now;
  if(header){leoPrefsRPC060.receive_header_valid=1;leoPrefsRPC060.received_id=header->msgh_id;leoPrefsRPC060.received_size=header->msgh_size;}
  OSMemoryBarrier();
 }
 ml_set_interrupts_enabled(irq);return seq;
}
uint32_t ios7leo_prefsrpc060_observe(LeoPrefsRPC060 *out)
{
 if(!out){return 0;}
 boolean_t irq=ml_set_interrupts_enabled(FALSE);*out=leoPrefsRPC060;ml_set_interrupts_enabled(irq);return out->known;
}
void ios7leo_prefsrpc060_stop(void){leoPrefsRPC060Stopped=1U;OSMemoryBarrier();}
#endif
