#ifndef IOS7_LEO_SWITCH_POLICY_H
#define IOS7_LEO_SWITCH_POLICY_H
#include <stdint.h>
enum { LEO_SWITCH_NONE=0,LEO_SWITCH_DEPRESS=1,LEO_SWITCH_WAIT=2,
 LEO_SWITCH_DISPATCH=3,LEO_SWITCH_OSLOCK_DEPRESS=4,LEO_SWITCH_OSLOCK_WAIT=5,
 LEO_SWITCH_RESTORE_WORKQ=0x100,LEO_SWITCH_MODE_MASK=3 };
typedef struct {int mode;uint32_t scale;int contention,same_task_hint;} LeoSwitchPolicy;
static inline int leo_switch_policy(int option,LeoSwitchPolicy *p){
 if(option<0||option>5)return 0;
 p->mode=option;p->scale=1000000U;p->contention=option>=3;p->same_task_hint=option>=4;
 if(option==3){p->mode=2;p->scale=1000U;}
 if(option==4)p->mode=1;
 if(option==5)p->mode=2;
 return 1;
}
#endif
