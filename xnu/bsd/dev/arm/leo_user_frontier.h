#ifndef LEO_ONE_EVENT_USER_FRONTIER_H
#define LEO_ONE_EVENT_USER_FRONTIER_H
#include <stdint.h>
#include <mach/arm/thread_status.h>
struct thread;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_frontier_arm(void);
void ios7leo_frontier_mach_entry(const arm_saved_state_t *);
void ios7leo_frontier_mach_return(const arm_saved_state_t *);
void ios7leo_frontier_bsd_entry(const arm_saved_state_t *,uint32_t);
void ios7leo_frontier_bsd_return(const arm_saved_state_t *,int);
void ios7leo_frontier_wait(struct thread *);
uint32_t ios7leo_frontier_take(uint8_t *,uint32_t,uint32_t *,uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
