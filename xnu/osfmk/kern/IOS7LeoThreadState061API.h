#ifndef IOS7LEO_THREADSTATE061_API_H
#define IOS7LEO_THREADSTATE061_API_H
#include "IOS7LeoThreadState061Shared.h"
void ios7leo_threadstate061_locked(thread_t,uint32_t);
void ios7leo_threadstate061_return(thread_t,uint32_t);
void ios7leo_threadstate061_irq(thread_t,const arm_saved_state_t *);
#endif
