#ifndef IOS7LEO_THREADSTATE061_SHARED_H
#define IOS7LEO_THREADSTATE061_SHARED_H
#include <stdint.h>
enum {LEO_TS061_ENTRY=1,LEO_TS061_HELD=2,LEO_TS061_STOPPED=3,LEO_TS061_INTERRUPTED=4,LEO_TS061_DONE=5};
enum {LEO_TS061_WAIT_SUSP=1,LEO_TS061_WAIT_RUN=2,LEO_TS061_BLOCK_RETURN=3,LEO_TS061_DISPATCH_RUN_WAKE=4,LEO_TS061_DISPATCH_CLEAR_RUN=5,LEO_TS061_DISPATCH_WAIT_WAKE=6,LEO_TS061_UNSTOP_CLEAR=7,LEO_TS061_UNSTOP_WAKE=8};
typedef struct LeoThreadState061 {
 uint64_t caller_tid,target_tid,begin_abs,phase_abs,event_abs,irq_abs,wait_event;
 uint32_t known,active,call,pid,version,target_ptr,same_task,flavor,request_count,phase,result;
 uint32_t held_known,suspend_count,user_stop_count;
 uint32_t event,event_count,state,sched_flags,ast,runq,processor,on_processor,wake_active,continuation;
 uint32_t wait_susp,wait_run,block_returns,wake_attempts,clear_runs,last_wresult;
 uint32_t irq_user,irq_kernel,irq_pc,irq_lr,irq_psr;
} LeoThreadState061;
uint32_t ios7leo_threadstate061_take(LeoThreadState061 *);
void ios7leo_threadstate061_stop(void);
#define LEO_TS061_META_FORMAT "PREFSTOP061 S=%u KN=%u ACTIVE=%u CALL=%u PID_VER=%u/%u CALLER_TARGET=%llx/%llx PTR=%x SAME=%u FLAVOR_COUNT=%x/%u PHASE=%u MR=%x HELD_KN_SUSP_USER=%u/%u/%u TICKS=%llu/%llu first-main-call-scope;counts-at-held\n"
#define LEO_TS061_STATE_FORMAT "PREFSTOP061 S=%u EVENT_N=%u/%u STATE_FLAGS_AST=%x/%x/%x RUNQ_PROC_CPU=%x/%x/%u WAKE=%u WAIT_CONT=%llx/%x COUNTERS=%u/%u/%u/%u/%u WRESULT=%x IRQ_U_K_PC_LR_PSR=%u/%u/%x/%x/%x TICKS=%llu/%llu published-events;wake-attempt-not-completion\n"
#endif
