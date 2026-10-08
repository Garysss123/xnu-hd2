#ifndef IOS7_LEO_ERROR_LOG_H
#define IOS7_LEO_ERROR_LOG_H
#include <stdint.h>
/* Return3 means a completed ordinary line for bounded wait classification, NOT an error.
 * Bounded streaming classifier. Newline terminates one logical record; partial
 * last lines are not durable. ASL Message hashes exclude its volatile envelope. */
typedef struct LeoErrorLine {
 char text[384],tail[32];
 uint32_t used,tail_used,truncated,matched,critical,ready,capture_gap;
 uint32_t hash,message,depth,escaped,pid,pid_digits,skip_space;
 uint32_t slash_odd,message_marker_escaped;
} LeoErrorLine;
static inline unsigned leo_error_lower(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
static inline unsigned leo_error_length(const char *s){unsigned n=0;while(s[n])++n;return n;}
static inline int leo_error_suffix(const LeoErrorLine *s,const char *word){
 unsigned n=leo_error_length(word);if(s->tail_used<n)return 0;
 for(unsigned i=0;i<n;i++)if(leo_error_lower((unsigned char)s->tail[s->tail_used-n+i])!=leo_error_lower((unsigned char)word[i]))return 0;
 if(s->tail_used>n){unsigned p=leo_error_lower((unsigned char)s->tail[s->tail_used-n-1]);if((p>='a'&&p<='z')||(p>='0'&&p<='9')||p=='_')return 0;}
 return 1;
}
static inline void leo_error_reset(LeoErrorLine *s){
 unsigned char *p=(unsigned char *)s;for(unsigned i=0;i<sizeof(*s);i++)p[i]=0;s->hash=2166136261U;
}
static inline int leo_error_feed(LeoErrorLine *s,unsigned char c){
 if(s->ready)leo_error_reset(s);
 if(c=='\r')return 0;
 int opened_message=0;
 if(c=='[')s->message_marker_escaped=s->slash_odd;
 s->slash_odd=c=='\\'?!s->slash_odd:0;
 if(s->tail_used==sizeof(s->tail)){for(unsigned i=1;i<sizeof(s->tail);i++)s->tail[i-1]=s->tail[i];--s->tail_used;}
 s->tail[s->tail_used++]=(char)c;
 if(s->pid_digits){if(c>='0'&&c<='9'){if(s->pid<=429496729U)s->pid=s->pid*10U+(c-'0');}else s->pid_digits=0;}
 if(leo_error_suffix(s,"[PID ")){s->pid=0;s->pid_digits=1;}
 if(!s->message&&!s->message_marker_escaped&&leo_error_suffix(s,"[Message ")){s->hash=2166136261U;s->message=1;s->depth=1;s->escaped=0;s->used=0;s->truncated=s->capture_gap?1U:0U;opened_message=1;}
 else if(s->message!=2){
  if(s->message==1){
   if(!s->escaped){if(c=='[')++s->depth;else if(c==']'&&--s->depth==0)s->message=2;}
   if(c=='\\')s->escaped=!s->escaped;else s->escaped=0;
  }
  if(s->message!=2){s->hash=(s->hash^c)*16777619U;}
 }
 int critical=leo_error_suffix(s,"panic(")||leo_error_suffix(s,"panic:")||leo_error_suffix(s,"fatal ")||leo_error_suffix(s,"exc_bad_")||leo_error_suffix(s,"assertion failed")||leo_error_suffix(s,"assertion failure")||leo_error_suffix(s,"out of memory");
 int error=critical||leo_error_suffix(s,"error:")||leo_error_suffix(s,"error ")||leo_error_suffix(s,"error\n")||leo_error_suffix(s,"failed:")||leo_error_suffix(s,"failed ")||leo_error_suffix(s,"failed\n")||leo_error_suffix(s,"timed out")||leo_error_suffix(s,"timeout ")||leo_error_suffix(s,"timeout\n")||leo_error_suffix(s,"uncaught exception");
 int copied_tail=0;
 if(error&&!s->matched&&s->truncated&&s->message!=2){s->used=0;for(unsigned i=0;i<s->tail_used&&s->used<sizeof(s->text)-1;i++)if(s->tail[i]!='\n')s->text[s->used++]=s->tail[i];copied_tail=1;}
 if(error){s->matched=1;}
 if(critical){s->critical=1;}
 if(c=='\n'){
  if(s->message==1)s->truncated=1;
  s->text[s->used]=0;
  if(s->used){s->ready=1;return s->matched?(s->critical?2:1):3;}
  leo_error_reset(s);return 0;
 }
 if(!copied_tail&&!opened_message&&s->message!=2){if(s->used<sizeof(s->text)-1)s->text[s->used++]=(char)c;else s->truncated=1;}
 return 0;
}
#include "LeoAXLogIdentity059.h"
static inline uint32_t leo_error_key(const LeoErrorLine *s){
 uint32_t identity=s->hash;
 (void)leo_ax059_identity(s->text,s->used,s->truncated,s->capture_gap,&identity);
 return (identity^s->pid)*16777619U;
}
typedef struct LeoErrorBudget {uint32_t normal[64],critical[16],normal_seen[64],critical_seen[16],normal_count,critical_count;} LeoErrorBudget;
static inline void leo_error_budget_reset(LeoErrorBudget *b){unsigned char *p=(unsigned char *)b;for(unsigned i=0;i<sizeof(*b);i++)p[i]=0;}
static inline int leo_error_admit(LeoErrorBudget *b,const LeoErrorLine *s,int kind){
 uint32_t key=leo_error_key(s),*keys=kind==2?b->critical:b->normal,*count=kind==2?&b->critical_count:&b->normal_count,cap=kind==2?16:64;
 uint32_t *seen=kind==2?b->critical_seen:b->normal_seen;
 for(uint32_t i=0;i<*count;i++)if(keys[i]==key){if(seen[i]!=UINT32_MAX)seen[i]++;return 0;}
 if(*count>=cap){return 0;}
 seen[*count]=1;keys[(*count)++]=key;return 1;
}
static inline uint32_t leo_error_occurrences(const LeoErrorBudget *b,const LeoErrorLine *s,int kind){
 uint32_t key=leo_error_key(s);const uint32_t *keys=kind==2?b->critical:b->normal,*seen=kind==2?b->critical_seen:b->normal_seen;uint32_t n=kind==2?b->critical_count:b->normal_count;
 for(uint32_t i=0;i<n;i++){if(keys[i]==key)return seen[i];}return 0;
}
#endif
