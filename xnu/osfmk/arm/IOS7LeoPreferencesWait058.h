#ifndef IOS7LEO_PREFERENCES_WAIT058_H
#define IOS7LEO_PREFERENCES_WAIT058_H
#include "IOS7LeoPreferencesWait058Shared.h"
/* Included by the existing fault/IRQ witness TU. One CPU, no allocation,
 * process references, user-memory reads, formatting or storage in the IRQ. */
typedef struct LeoPrefs058Store {
 volatile uint32_t ready, done;
 uint32_t pid, pidversion, irq_count, irq_pc, irq_sp, irq_lr, irq_cpsr, irq_r7;
 uint64_t tid, first_abs, irq_abs;
} LeoPrefs058Store;
static LeoPrefs058Store leoPrefs058;
static uint32_t leoPrefs058Stage;
static const uint32_t leoPrefs058Seconds[10]={0,1,5,15,30,45,60,90,120,150};
typedef char leo_prefs058_storage_bound[(sizeof(leoPrefs058)<=128U && sizeof(LeoPrefs058Snapshot)<=256U)?1:-1];

static void leo_prefs058_irq(thread_t t,const arm_saved_state_t *r)
{
 if(leoPrefs058.done || !t || !r || (r->cpsr&31U)!=16U ||
    !(thread_get_tag_internal(t)&THREAD_TAG_MAINTHREAD) || !t->task || !t->task->bsd_info)return;
 if(leoPrefs058.ready){
  if(t->thread_id!=leoPrefs058.tid || (uint32_t)proc_pid(t->task->bsd_info)!=leoPrefs058.pid)return;
 }else{
  const char *name=proc_name_address(t->task->bsd_info);
  int pid=proc_pid(t->task->bsd_info);
  if(!name || strncmp(name,"Preferences",12U) || pid<=0)return;
  leoPrefs058.pid=(uint32_t)pid;leoPrefs058.tid=t->thread_id;
  leoPrefs058.pidversion=(uint32_t)proc_pidversion(t->task->bsd_info);
  leoPrefs058.first_abs=mach_absolute_time();
 }
 if(leoPrefs058.irq_count!=UINT32_MAX)leoPrefs058.irq_count++;
 leoPrefs058.irq_pc=r->pc;leoPrefs058.irq_sp=r->sp;leoPrefs058.irq_lr=r->lr;
 leoPrefs058.irq_cpsr=r->cpsr;leoPrefs058.irq_r7=r->r[7];
 leoPrefs058.irq_abs=mach_absolute_time();OSMemoryBarrier();leoPrefs058.ready=1U;
}

uint32_t ios7leo_prefs058_take(uint64_t now_ns,LeoPrefs058Snapshot *out)
{
 LeoPrefs058Store snap;uint64_t first_ns=0;boolean_t enabled;
 leo_prefs_proc_t p;task_t task;thread_t th=THREAD_NULL,it;
 if(!out || leoPrefs058Stage>=10U || !leoPrefs058.ready)return 0U;
 /* Only a small resident struct is copied under the original CPU IRQ mask. */
 enabled=ml_set_interrupts_enabled(FALSE);snap=leoPrefs058;ml_set_interrupts_enabled(enabled);
 absolutetime_to_nanoseconds(snap.first_abs,&first_ns);
 /* The IRQ may arm after the worker fetched now_ns. That is not a clock
  * reversal: leave the schedule untouched and use the next worker time. */
 if(now_ns<first_ns || now_ns-first_ns<(uint64_t)leoPrefs058Seconds[leoPrefs058Stage]*1000000000ULL)return 0U;
 bzero(out,sizeof(*out));out->first_ns=first_ns;out->now_ns=now_ns;
 out->ordinal=leoPrefs058Stage+1U;out->requested=leoPrefs058Seconds[leoPrefs058Stage++];
 out->pid=snap.pid;out->tid=snap.tid;out->irq_count=snap.irq_count;out->irq_abs=snap.irq_abs;
 out->irq_pc=snap.irq_pc;out->irq_sp=snap.irq_sp;out->irq_lr=snap.irq_lr;
 out->irq_cpsr=snap.irq_cpsr;out->irq_r7=snap.irq_r7;
 if(leoPrefs058Stage==10U)leoPrefs058.done=1U;
 /* proc_find has a normal process-list lock: not a try-only or wall-time-
  * bounded operation. It is called ONLY in the ordinary logger worker,
  * before any storage access and with no observer/IRQ/device lock held. */
 p=proc_find((int)snap.pid);
 if(!p){out->status=LEO_PREFS058_NO_PROC;return 1U;}
 if(strncmp(proc_name_address(p),"Preferences",12U) || (uint32_t)proc_pidversion(p)!=snap.pidversion){out->status=LEO_PREFS058_IDENTITY;proc_rele(p);return 1U;}
 task=proc_task(p);
 if(!task){out->status=LEO_PREFS058_NO_TASK;proc_rele(p);return 1U;}
 /* The proc reference prevents exit's task teardown until after snapshot.
  * After the observed user IRQ, ordinary exec keeps this legacy task object. */
 task_reference(task);
 if(!task_lock_try(task)){out->status=LEO_PREFS058_TASK_BUSY;task_deallocate(task);proc_rele(p);return 1U;}
 queue_iterate(&task->threads,it,thread_t,task_threads){
  if(out->threads_examined>=64U)break;
  out->threads_examined++;
  if(it->thread_id==snap.tid && (thread_get_tag_internal(it)&THREAD_TAG_MAINTHREAD)){
   th=it;thread_reference(th);break;
  }
 }
 task_unlock(task);task_deallocate(task);
 if(th==THREAD_NULL){out->status=LEO_PREFS058_NO_THREAD;proc_rele(p);return 1U;}
 if(!thread_mtx_try(th)){out->status=LEO_PREFS058_THREAD_BUSY;thread_deallocate(th);proc_rele(p);return 1U;}
 enabled=ml_set_interrupts_enabled(FALSE);
 if(!simple_lock_try(&th->sched_lock)){
  ml_set_interrupts_enabled(enabled);thread_mtx_unlock(th);
  out->status=LEO_PREFS058_SCHED_BUSY;thread_deallocate(th);proc_rele(p);return 1U;
 }
 /* One-CPU IRQ exclusion prevents concurrent execution/exec while copying.
  * References protect lifetime; no stopping/suspending/resuming the target. */
 out->active=th->active?1U:0U;out->state=(uint32_t)th->state;
 out->wait_event=(uint64_t)th->wait_event;out->wait_result=(uint32_t)th->wait_result;
 out->continuation=(uint32_t)(uintptr_t)th->continuation;
 /* pidversion changes during exec but is not an in-transit flag. A syscall
  * frame observed before that increment remains saved state, not proof that
  * application initialization has finished. No remote addresses are read. */
 if(strncmp(proc_name_address(p),"Preferences",12U) || proc_exiting(p) ||
    (uint32_t)proc_pidversion(p)!=snap.pidversion)out->status=LEO_PREFS058_TRANSITION;
 else if(!th->active || (th->state&TH_TERMINATE))out->status=LEO_PREFS058_INACTIVE;
 else if(!(th->state&TH_WAIT) || (th->state&TH_RUN))out->status=LEO_PREFS058_OTHER_STATE;
 else if(!th->machine.uss)out->status=LEO_PREFS058_NO_USS;
 else{
  const arm_saved_state_t *u=th->machine.uss;
  for(unsigned i=0;i<13U;i++)out->r[i]=u->r[i];
  out->sp=u->sp;out->lr=u->lr;out->pc=u->pc;out->cpsr=u->cpsr;
  out->uss_valid=((u->cpsr&31U)==16U)?1U:0U;
  out->status=out->uss_valid?LEO_PREFS058_WAIT:LEO_PREFS058_NO_USS;
 }
 thread_unlock(th);ml_set_interrupts_enabled(enabled);thread_mtx_unlock(th);
 thread_deallocate(th);proc_rele(p);return 1U;
}
#endif
