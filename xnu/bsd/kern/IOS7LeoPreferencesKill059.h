#ifndef IOS7LEO_PREFERENCES_KILL059_H
#define IOS7LEO_PREFERENCES_KILL059_H
/* Included in the actual BSD signal TU. Referenced caller/target already exist.
 * Only observation; no changed permission, signal delivery, mask or status. */
typedef struct LeoPrefKill059Record {
 volatile UInt32 ready;uint32_t source,caller,target,signal;uint64_t absolute;
} LeoPrefKill059Record;
static LeoPrefKill059Record leoPrefKill059[8];
static volatile UInt32 leoPrefKill059Count;
static void leo_prefkill059_note(uint32_t source,proc_t caller,proc_t target,int signal)
{
 if(target==PROC_NULL||signal<=0||signal>=NSIG||strncmp(target->p_comm,"Preferences",12U))return;
 UInt32 n=leoPrefKill059Count;
 if(n>=8U||!OSCompareAndSwap(n,n+1U,&leoPrefKill059Count))return;
 LeoPrefKill059Record *r=&leoPrefKill059[n];
 r->source=source;r->caller=caller?caller->p_pid:0;r->target=target->p_pid;
 r->signal=(uint32_t)signal;r->absolute=mach_absolute_time();
 OSMemoryBarrier();r->ready=1U;
}
uint32_t ios7leo_prefkill059_take(uint8_t *payload,uint32_t capacity)
{
 if(!payload||capacity<256U)return 0;
 for(unsigned i=0;i<8U;i++){
  LeoPrefKill059Record *r=&leoPrefKill059[i];if(r->ready!=1U)continue;
  OSMemoryBarrier();
  int n=snprintf((char *)payload,capacity,"PREFKILL059 SEQ=%u SOURCE=%u CALLER=%u TARGET=%u SIGNAL=%u REQUEST_ABS=%llu src1=kill-request,2=psignal,3=kill-return,4=suspend-enter,5=suspend-return;not-final-exit\n",i+1U,r->source,r->caller,r->target,r->signal,(unsigned long long)r->absolute);
  if(n<=0||(uint32_t)n>=capacity)return 0;
  if(OSCompareAndSwap(1U,2U,&r->ready))return (uint32_t)n;
 }
 return 0;
}
#endif
