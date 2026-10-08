#ifndef IOS7LEO_PREFERENCES_WAIT058_SHARED_H
#define IOS7LEO_PREFERENCES_WAIT058_SHARED_H
#include <stdint.h>
/* Status is evidence availability, never an app-health verdict. */
enum { LEO_PREFS058_WAIT=1, LEO_PREFS058_OTHER_STATE=2,
 LEO_PREFS058_NO_PROC=3, LEO_PREFS058_IDENTITY=4, LEO_PREFS058_NO_TASK=5,
 LEO_PREFS058_TASK_BUSY=6, LEO_PREFS058_NO_THREAD=7, LEO_PREFS058_THREAD_BUSY=8,
 LEO_PREFS058_SCHED_BUSY=9, LEO_PREFS058_INACTIVE=10, LEO_PREFS058_NO_USS=11,
 LEO_PREFS058_TRANSITION=12, LEO_PREFS058_CLOCK_BACK=13 };
typedef struct LeoPrefs058Snapshot {
 uint64_t first_ns, now_ns, tid, irq_abs, wait_event;
 uint32_t ordinal, requested, pid, status, active, state, wait_result;
 uint32_t continuation, uss_valid, threads_examined, irq_count;
 uint32_t irq_pc, irq_sp, irq_lr, irq_cpsr, irq_r7;
 uint32_t r[13], sp, lr, pc, cpsr;
} LeoPrefs058Snapshot;
/* Existing logger worker only. Zero means no due observation. */
uint32_t ios7leo_prefs058_take(uint64_t now_ns, LeoPrefs058Snapshot *out);
#define LEO_PREFS058_META_FORMAT "PREFWAIT058 S=%u REQ=%u FIRST_NS=%llu NOW_NS=%llu PID=%u TID=%llx STATUS=%u ACTIVE_STATE=%u/%x WAIT=%llx WR=%x CONT=%x USS=%u SCAN=%u IRQ_N=%u IRQ_ABS=%llu LAST_IRQ_PC_SP_LR_PSR_R7=%x/%x/%x/%x/%x first-IRQ-anchor;not-exec-time-or-health\n"
#define LEO_PREFS058_REG_FORMAT "PREFREG058 S=%u PID=%u TID=%llx VALID=%u PC_SP_LR_PSR=%x/%x/%x/%x R0_R12=%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x blocked-saved-user-state;not-user-stack\n"
#endif
