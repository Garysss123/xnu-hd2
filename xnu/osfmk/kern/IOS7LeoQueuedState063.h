#ifndef IOS7LEO_QUEUED_STATE063_H
#define IOS7LEO_QUEUED_STATE063_H
#include "IOS7LeoQueuedState063Shared.h"
/* Called only with the original target mutex held and active checked.
 * A non-current single-CPU target cannot execute while removed from its
 * normal run queue under sched/IRQ protection. Keep TH_RUN and all logical
 * running/share/workqueue counts intact. No synthetic hold/block/unblock.
 * ARM_THREAD_STATE's existing machine getter copies exactly17 resident words.
 * A prior dispatch does not make saved user registers live: USS is resident,
 * distinct from the kernel continuation, and the target mutex prevents a
 * competing state setter. Read only with no pre-existing suspension owner.
 * Use a local buffer so caller buffers are written only after IRQ restoration.
 */
static LeoQueuedState063 leoQueuedState063;
static void leo_qs063_inc(uint32_t *n){if(*n!=UINT32_MAX)(*n)++;}
typedef char leo_qs063_arm_count[(ARM_THREAD_STATE_COUNT==17)?1:-1];
static boolean_t leo_queued_state063_try(thread_t target,int flavor,
 thread_state_t output,mach_msg_type_number_t *output_count,kern_return_t *result)
{
 natural_t local[ARM_THREAD_STATE_COUNT];mach_msg_type_number_t count;
 boolean_t handled=FALSE;thread_t self=current_thread();spl_t irq;
 if(real_ncpus!=1U||flavor!=ARM_THREAD_STATE||!target||target==self||!output||!output_count||!result)return FALSE;
 count=*output_count; /* Caller memory is accessed before IRQ masking. */
 irq=splsched();thread_lock(target);leo_qs063_inc(&leoQueuedState063.attempts);
 if(target->active&&target->state==TH_RUN&&
    (target->last_processor==PROCESSOR_NULL||target->last_processor==&BootProcessor)&&
    target->suspend_count==0&&target->user_stop_count==0&&
    target->runq==&BootProcessor&&BootProcessor.active_thread!=target&&
    (target->sched_mode==TH_MODE_FIXED||target->sched_mode==TH_MODE_TIMESHARE)&&
    target->sched_pri>=0&&target->sched_pri<BASEPRI_RTQUEUES&&target->machine.uss){
  leo_qs063_inc(&leoQueuedState063.eligible);
  if(thread_run_queue_remove(target)){
   leo_qs063_inc(&leoQueuedState063.removed);
   leoQueuedState063.caller_tid=self->thread_id;leoQueuedState063.target_tid=target->thread_id;
   leoQueuedState063.last_state=target->state;leoQueuedState063.last_priority=(uint32_t)target->sched_pri;
   leoQueuedState063.last_base_priority=(uint32_t)target->priority;
   *result=machine_thread_get_state(target,ARM_THREAD_STATE,local,&count);
   leoQueuedState063.last_count=count;leoQueuedState063.last_result=(uint32_t)*result;
   /* Mandatory even for a short-count/backend error. Original API updates
    * queue bookkeeping/processor selection; no hand-edited policy or counts. */
   thread_setrun(target,SCHED_PREEMPT|SCHED_TAILQ);
   leo_qs063_inc(&leoQueuedState063.requeued);
   if(*result==KERN_SUCCESS)leo_qs063_inc(&leoQueuedState063.success);
   else leo_qs063_inc(&leoQueuedState063.backend_error);
   handled=TRUE;
  }else leo_qs063_inc(&leoQueuedState063.remove_failed);
 }
 OSMemoryBarrier();thread_unlock(target);splx(irq);
 if(handled&&*result==KERN_SUCCESS){
  for(unsigned i=0;i<ARM_THREAD_STATE_COUNT;i++)output[i]=local[i];
  *output_count=count;
 }
 return handled;
}
uint32_t ios7leo_queuedstate063_observe(LeoQueuedState063 *out)
{
 if(!out){return 0;}
 boolean_t irq=ml_set_interrupts_enabled(FALSE);
 *out=leoQueuedState063;ml_set_interrupts_enabled(irq);return 1;
}
#endif
