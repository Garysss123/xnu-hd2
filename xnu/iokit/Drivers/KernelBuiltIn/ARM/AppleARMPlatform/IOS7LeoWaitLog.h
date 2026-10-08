#ifndef IOS7_LEO_WAIT_LOG_H
#define IOS7_LEO_WAIT_LOG_H
#include <stdint.h>
#include "IOS7LeoErrorLog.h"
#define LEO_WAIT_RECORD_FORMAT "WAITDIAG WHY=%u PID=%u KEY=%x MSGS=%u FLAGS=%x REPORTED_RETRY=%u/%u PAIR=%u ID=%u SPAN=%llu WAIT=%llu GAP=%u DROP=%u BACK=%u TRUNC=%u %.80s [msgs/worker-time;not-attempts/hang-proof]\n"
/* Worker-only, fixed-size diagnostics. Counts are observed log messages, not
 * proven retry attempts. Paired waits require a reported PID and wait_id.
 * Observation intervals are not function execution time. No I/O or clocks here. */
enum { LEO_W_RETRY=1,LEO_W_WAIT=2,LEO_W_BEGIN=4,LEO_W_END=8,LEO_W_COUNTER=16 };
enum { LEO_W_REPEAT=1,LEO_W_LONG=2,LEO_W_CLOSED=3,LEO_W_REPORTED=4,LEO_W_LIMIT=5,LEO_W_OVERLAP=6 };
typedef struct LeoWaitEntry {
 uint32_t used,key,pid,flags,count,next_count,reported_retry,reported_valid;
 uint32_t paired,wait_id,open,stage,ambiguous,closed_pending,source_trunc,gap_epoch;
 uint32_t emitted_retry,emitted_valid,closed_count,begin_without_end,begin_next;
 uint64_t first_ns,last_ns,begin_ns,closed_span_ns;
 char text[112],closed_text[112];
} LeoWaitEntry;
typedef struct LeoWaitState {
 LeoWaitEntry entries[16];
 uint32_t records,cursor,untracked_messages,gaps,limit_reported,clock_backwards;
 uint32_t have_now,have_emit;uint64_t last_now,last_emit;
} LeoWaitState;
typedef struct LeoWaitReport {
 uint32_t reason,key,pid,count,flags,reported_retry,reported_valid,wait_id,paired;
 uint32_t source_trunc,gaps,untracked_messages,clock_backwards;
 uint64_t tracked_span_ns,observed_wait_ns;
 char text[112];
} LeoWaitReport;
static inline void leo_wait_zero(void *p,unsigned n){unsigned char *b=(unsigned char *)p;for(unsigned i=0;i<n;i++)b[i]=0;}
static inline unsigned leo_wait_lower(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
static inline int leo_wait_word(unsigned c){c=leo_wait_lower(c);return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}
static inline const char *leo_wait_find(const char *s,const char *word){
 for(unsigned i=0;s[i];i++){
  if(i&&leo_wait_word((unsigned char)s[i-1]))continue;
  unsigned j=0;while(word[j]&&s[i+j]&&leo_wait_lower((unsigned char)s[i+j])==leo_wait_lower((unsigned char)word[j]))j++;
  if(!word[j])return s+i+j;
 }return 0;
}
static inline int leo_wait_number(const char *s,const char *key,uint32_t *value){
 const char *p=leo_wait_find(s,key);if(!p||*p<'0'||*p>'9')return 0;
 uint32_t v=0;unsigned digits=0;
 while(*p>='0'&&*p<='9'){uint32_t d=(uint32_t)(*p-'0');if(v>(UINT32_MAX-d)/10U)return 0;v=v*10U+d;p++;if(++digits>10)return 0;}
 if(leo_wait_word((unsigned char)*p)||*p=='.'){return 0;}*value=v;return 1;
}
static inline uint32_t leo_wait_flags(const char *s){
 uint32_t f=0;
 if(leo_wait_find(s,"retry ")||leo_wait_find(s,"retry:")||leo_wait_find(s,"retrying "))f|=LEO_W_RETRY;
 if(leo_wait_find(s,"waiting for ")||leo_wait_find(s,"waiting on ")||leo_wait_find(s,"still waiting"))f|=LEO_W_WAIT;
 if(leo_wait_find(s,"wait-begin ")||leo_wait_find(s,"wait_begin "))f|=LEO_W_BEGIN|LEO_W_WAIT;
 if(leo_wait_find(s,"wait-end ")||leo_wait_find(s,"wait_end "))f|=LEO_W_END;
 if(leo_wait_find(s,"retry_count=")||leo_wait_find(s,"retries="))f|=LEO_W_COUNTER;
 return f;
}
static inline void leo_wait_reset(LeoWaitState *s){leo_wait_zero(s,sizeof(*s));}
static inline void leo_wait_gap(LeoWaitState *s){
 if(s->gaps!=UINT32_MAX)s->gaps++;
 for(unsigned i=0;i<16;i++){s->entries[i].open=0;s->entries[i].closed_pending=0;s->entries[i].ambiguous=1;}
}
static inline int leo_wait_time(LeoWaitState *s,uint64_t now){
 if(s->have_now&&now<s->last_now){if(s->clock_backwards!=UINT32_MAX)s->clock_backwards++;leo_wait_gap(s);s->have_emit=1;s->last_emit=now;s->last_now=now;return 0;}
 s->last_now=now;s->have_now=1;return 1;
}
static inline uint32_t leo_wait_excerpt(char dst[112],const LeoErrorLine *line){
 const char *p=line->text;
 unsigned i=0;for(;i<111&&p[i]&&p[i]!='\n'&&p[i]!='\r';i++){unsigned c=(unsigned char)p[i];dst[i]=c<32?' ':(char)c;}dst[i]=0;return p[i]&&p[i]!='\n'&&p[i]!='\r';
}
static inline unsigned leo_wait_at(const char *s,const char *key){unsigned n=0;while(key[n]&&s[n]&&leo_wait_lower((unsigned char)s[n])==leo_wait_lower((unsigned char)key[n]))n++;return key[n]?0:n;}
/* Only explicit retry counter digits are normalized; error codes/objects remain
 * part of the key. Long/truncated lines retain the full streaming hash instead. */
static inline uint32_t leo_wait_counter_key(const LeoErrorLine *line){
 if(line->truncated)return leo_error_key(line);
 const char *p=line->text;unsigned previous=0;
 uint32_t h=2166136261U;
 while(*p){
  unsigned c=(unsigned char)*p;
  unsigned n=!leo_wait_word(previous)?leo_wait_at(p,"retry_count="):0;
  if(!n&&!leo_wait_word(previous))n=leo_wait_at(p,"retries=");
  if(n){for(unsigned i=0;i<n;i++)h=(h^(unsigned char)*p++)*16777619U;while(*p>='0'&&*p<='9')p++;h=(h^'#')*16777619U;previous='#';continue;}
  h=(h^c)*16777619U;previous=c;p++;
 }return (h^line->pid)*16777619U;
}
static inline void leo_wait_note(LeoWaitState *s,const LeoErrorLine *line,int kind,uint32_t error_occurrences,uint64_t now){
 if(line->capture_gap||!leo_wait_time(s,now))return;
 uint32_t flags=leo_wait_flags(line->text),reported=0,id=0;
 int has_reported=leo_wait_number(line->text,"retry_count=",&reported)||leo_wait_number(line->text,"retries=",&reported);
 if(flags==LEO_W_COUNTER&&(!has_reported||reported<3))return;
 if(!flags&&(kind==3||error_occurrences<3))return;
 int pair=!line->truncated&&line->pid&&leo_wait_number(line->text,"wait_id=",&id)&&(flags&(LEO_W_BEGIN|LEO_W_END));
 uint32_t key=pair?((id^line->pid)*16777619U)^0x57414954U:has_reported?leo_wait_counter_key(line):leo_error_key(line);
 unsigned first=flags?0:8,last=flags?8:16,index=last,empty=last;
 for(unsigned i=first;i<last;i++){
  LeoWaitEntry *e=&s->entries[i];if(!e->used){if(empty==last)empty=i;continue;}
  if(e->key==key&&e->pid==line->pid&&e->paired==(uint32_t)pair&&(!pair||e->wait_id==id)){index=i;break;}
 }
 if(index==last){
  if(flags&LEO_W_END)return;
  if(empty==last){if(s->untracked_messages!=UINT32_MAX)s->untracked_messages++;return;}
  index=empty;LeoWaitEntry *e=&s->entries[index];leo_wait_zero(e,sizeof(*e));
  e->used=1;e->key=key;e->pid=line->pid;e->paired=(uint32_t)pair;e->wait_id=id;e->first_ns=now;e->next_count=3;
 }
 LeoWaitEntry *e=&s->entries[index];e->last_ns=now;e->flags|=flags;e->source_trunc|=line->truncated;e->gap_epoch=s->gaps;
 if(flags){if(e->count!=UINT32_MAX)e->count++;}else if(error_occurrences>e->count)e->count=error_occurrences;
 if(has_reported&&reported>=3&&(!e->emitted_valid||reported!=e->emitted_retry)){e->reported_retry=reported;e->reported_valid=1;}
 e->source_trunc|=leo_wait_excerpt(e->text,line);
 if(pair&&(flags&LEO_W_BEGIN)){
  if(!e->begin_without_end)e->begin_next=3;
  if(e->begin_without_end!=UINT32_MAX)e->begin_without_end++;
  if(e->open){e->open=0;e->ambiguous=1;}else if(!e->ambiguous){e->begin_ns=now;e->open=1;e->stage=0;}
 }
 if(pair&&(flags&LEO_W_END)){
  if(e->open&&now>=e->begin_ns){
   if(e->closed_pending&&s->untracked_messages!=UINT32_MAX)s->untracked_messages++;
   e->closed_span_ns=now-e->begin_ns;e->closed_pending=e->stage!=0||e->closed_span_ns>=30000000000ULL;e->closed_count=e->count;
   for(unsigned i=0;i<sizeof(e->closed_text);i++)e->closed_text[i]=e->text[i];
  }
  e->open=0;e->ambiguous=0;e->begin_without_end=0;
 }
}
static inline int leo_wait_poll(LeoWaitState *s,uint64_t now,LeoWaitReport *out){
 if(!leo_wait_time(s,now))return 0;
 if(s->have_emit&&now-s->last_emit<1000000000ULL)return 0;
 if((s->records>=32||s->untracked_messages)&&!s->limit_reported){
  leo_wait_zero(out,sizeof(*out));out->reason=LEO_W_LIMIT;out->untracked_messages=s->untracked_messages;out->count=s->records;out->gaps=s->gaps;out->clock_backwards=s->clock_backwards;
  s->limit_reported=1;s->last_emit=now;s->have_emit=1;return 1;
 }
 if(s->records>=32)return 0;
 for(unsigned n=0;n<16;n++){
  unsigned i=(s->cursor+n)%16;LeoWaitEntry *e=&s->entries[i];if(!e->used)continue;
  uint32_t reason=0;uint64_t age=e->open&&now>=e->begin_ns?now-e->begin_ns:0;
  if(e->closed_pending){reason=LEO_W_CLOSED;e->closed_pending=0;age=e->closed_span_ns;}
  else if(e->paired&&e->ambiguous&&e->begin_next&&e->begin_without_end>=e->begin_next){
   reason=LEO_W_OVERLAP;
   do{e->begin_next=e->begin_next==3?10:e->begin_next==10?100:e->begin_next==100?1000:0;}while(e->begin_next&&e->begin_next<=e->begin_without_end);
  }
  else if(e->open&&((!e->stage&&age>=30000000000ULL)||(e->stage==1&&age>=120000000000ULL))){reason=LEO_W_LONG;e->stage++;}
  else if(e->reported_valid){reason=LEO_W_REPORTED;e->reported_valid=0;e->emitted_retry=e->reported_retry;e->emitted_valid=1;}
  else if(!e->paired&&e->count>=e->next_count&&e->next_count){
   reason=LEO_W_REPEAT;
   do{e->next_count=e->next_count==3?10:e->next_count==10?100:e->next_count==100?1000:0;}while(e->next_count&&e->next_count<=e->count);
  }
  if(!reason)continue;
  leo_wait_zero(out,sizeof(*out));out->reason=reason;out->key=e->key;out->pid=e->pid;out->count=e->count;out->flags=e->flags;out->reported_retry=e->reported_retry;out->reported_valid=reason==LEO_W_REPORTED;out->wait_id=e->wait_id;out->paired=e->paired;out->source_trunc=e->source_trunc;out->gaps=s->gaps;out->untracked_messages=s->untracked_messages;out->clock_backwards=s->clock_backwards;out->tracked_span_ns=e->last_ns>=e->first_ns?e->last_ns-e->first_ns:0;out->observed_wait_ns=age;
  if(reason==LEO_W_CLOSED)out->count=e->closed_count;
  if(reason==LEO_W_OVERLAP)out->count=e->begin_without_end;
  for(unsigned k=0;k<sizeof(out->text);k++)out->text[k]=reason==LEO_W_CLOSED?e->closed_text[k]:e->text[k];
  s->cursor=(i+1)%16;s->records++;s->last_emit=now;s->have_emit=1;return 1;
 }return 0;
}
#endif
