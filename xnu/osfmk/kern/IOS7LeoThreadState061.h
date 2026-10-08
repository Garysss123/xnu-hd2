#ifndef IOS7LEO_THREADSTATE061_H
#define IOS7LEO_THREADSTATE061_H
#include "IOS7LeoThreadState061API.h"
#include "IOS7LeoPreferencesRPC060Shared.h"
/* Single-CPU scalar publication only. No target pointer is dereferenced by
 * the consumer; no new locks, reference, stack read, allocation or I/O. The
 * active original get_state invocation owns the target's normal MIG ref. */
static LeoThreadState061 leoThreadState061;
static volatile uint32_t leoThreadState061Stopped;
typedef char leo_ts061_bound[(sizeof(LeoThreadState061)<=256U)?1:-1];
static void leo_ts061_inc(uint32_t *n){if(*n!=UINT32_MAX)(*n)++;}
static uint32_t leo_ts061_begin(thread_t target,int flavor,uint32_t count)
{
 thread_t self=current_thread();LeoPrefsRPC060 rpc;
 if(leoThreadState061Stopped||!target||!self||!(thread_get_tag_internal(self)&THREAD_TAG_MAINTHREAD))return 0;
 bzero(&rpc,sizeof(rpc));ios7leo_prefsrpc060_observe(&rpc);
 /* The existing bounded RPC owner has already identified exact Preferences
  * main identity. Require this real send to be the native thread_get_state. */
 if(!rpc.known||rpc.tid!=self->thread_id||rpc.message_id!=3603||rpc.stage!=LEO_RPC060_COPYIN)return 0;
 uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);
 bzero(&leoThreadState061,sizeof(leoThreadState061));
 leoThreadState061.known=leoThreadState061.active=1;leoThreadState061.call=rpc.call;
 leoThreadState061.pid=rpc.pid;leoThreadState061.version=rpc.pidversion;
 leoThreadState061.caller_tid=self->thread_id;leoThreadState061.target_tid=target->thread_id;
 leoThreadState061.target_ptr=(uint32_t)(uintptr_t)target;leoThreadState061.same_task=(target->task==self->task);
 leoThreadState061.flavor=(uint32_t)flavor;leoThreadState061.request_count=count;
 leoThreadState061.begin_abs=leoThreadState061.phase_abs=now;leoThreadState061.phase=LEO_TS061_ENTRY;
 OSMemoryBarrier();ml_set_interrupts_enabled(irq);return rpc.call;
}
static void leo_ts061_phase(uint32_t call,thread_t target,uint32_t phase,uint32_t result)
{
 if(!call||leoThreadState061Stopped)return;
 uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);
 if(leoThreadState061.active&&leoThreadState061.call==call&&leoThreadState061.target_ptr==(uint32_t)(uintptr_t)target){
  leoThreadState061.phase=phase;leoThreadState061.phase_abs=now;leoThreadState061.result=result;
  /* Only HELD is called while the original target mutex is owned. */
  if(phase==LEO_TS061_HELD){leoThreadState061.held_known=1;leoThreadState061.suspend_count=target->suspend_count;leoThreadState061.user_stop_count=target->user_stop_count;}
  if(phase==LEO_TS061_DONE)leoThreadState061.active=0;
  OSMemoryBarrier();
 }
 ml_set_interrupts_enabled(irq);
}
void ios7leo_threadstate061_locked(thread_t target,uint32_t event)
{
 if(leoThreadState061Stopped||!leoThreadState061.active||leoThreadState061.target_ptr!=(uint32_t)(uintptr_t)target||leoThreadState061.target_tid!=target->thread_id)return;
 /* Call sites already own target scheduler/wake locks and mask interrupts.
  * Do not take a mutex, call clock/proc/port services, or emit text here. */
 leoThreadState061.event=event;leo_ts061_inc(&leoThreadState061.event_count);
 leoThreadState061.state=target->state;leoThreadState061.sched_flags=target->sched_flags;leoThreadState061.ast=target->ast;
 leoThreadState061.runq=(uint32_t)(uintptr_t)target->runq;leoThreadState061.processor=(uint32_t)(uintptr_t)target->last_processor;
 leoThreadState061.on_processor=target->last_processor&&target->last_processor->active_thread==target;
 leoThreadState061.wake_active=target->wake_active;leoThreadState061.wait_event=(uint64_t)target->wait_event;leoThreadState061.continuation=(uint32_t)(uintptr_t)target->continuation;
 if(event==LEO_TS061_WAIT_SUSP)leo_ts061_inc(&leoThreadState061.wait_susp);
 if(event==LEO_TS061_WAIT_RUN)leo_ts061_inc(&leoThreadState061.wait_run);
 if(event==LEO_TS061_DISPATCH_RUN_WAKE||event==LEO_TS061_DISPATCH_WAIT_WAKE||event==LEO_TS061_UNSTOP_WAKE)leo_ts061_inc(&leoThreadState061.wake_attempts);
 if(event==LEO_TS061_DISPATCH_CLEAR_RUN)leo_ts061_inc(&leoThreadState061.clear_runs);
 OSMemoryBarrier();
}
void ios7leo_threadstate061_return(thread_t target,uint32_t result)
{
 if(leoThreadState061Stopped||!leoThreadState061.active||leoThreadState061.target_ptr!=(uint32_t)(uintptr_t)target)return;
 uint64_t now=mach_absolute_time();boolean_t irq=ml_set_interrupts_enabled(FALSE);
 if(leoThreadState061.active&&leoThreadState061.target_ptr==(uint32_t)(uintptr_t)target){
  leoThreadState061.event=LEO_TS061_BLOCK_RETURN;leoThreadState061.last_wresult=result;leoThreadState061.event_abs=now;
  leo_ts061_inc(&leoThreadState061.event_count);leo_ts061_inc(&leoThreadState061.block_returns);OSMemoryBarrier();
 }
 ml_set_interrupts_enabled(irq);
}
void ios7leo_threadstate061_irq(thread_t thread,const arm_saved_state_t *r)
{
 if(leoThreadState061Stopped||!leoThreadState061.active||!thread||!r||leoThreadState061.target_ptr!=(uint32_t)(uintptr_t)thread||leoThreadState061.target_tid!=thread->thread_id)return;
 if((r->cpsr&31U)==16U)leo_ts061_inc(&leoThreadState061.irq_user);else leo_ts061_inc(&leoThreadState061.irq_kernel);
 leoThreadState061.irq_pc=r->pc;leoThreadState061.irq_lr=r->lr;leoThreadState061.irq_psr=r->cpsr;leoThreadState061.irq_abs=mach_absolute_time();OSMemoryBarrier();
}
uint32_t ios7leo_threadstate061_take(LeoThreadState061 *out)
{
 if(!out)return 0;
 boolean_t irq=ml_set_interrupts_enabled(FALSE);*out=leoThreadState061;ml_set_interrupts_enabled(irq);return out->known;
}
void ios7leo_threadstate061_stop(void){leoThreadState061Stopped=1;OSMemoryBarrier();}
#endif
