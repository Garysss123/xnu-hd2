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
struct ios7leo_service_activity {
 uint32_t pid,requests,replies,last_request,last_reply,status_kind,status,disposition,saturated,service_pairs,limit_reached,late_wait,loss,unknown;
 uint64_t tid,last_tick;
};
uint32_t ios7leo_frontier_activity(struct ios7leo_service_activity *);
uint32_t ios7leo_frontier_service_begin(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
void ios7leo_frontier_service_reply(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
uint32_t ios7leo_frontier_take(uint8_t *,uint32_t,uint32_t *,uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
