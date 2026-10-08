#ifndef LEO_AX_LOG_IDENTITY059_H
#define LEO_AX_LOG_IDENTITY059_H
#include <stdint.h>
/* Deduplicate only volatile object addresses in the two observed AX message
 * forms. The original message is retained for output; codes, ports, service,
 * key and payload all remain in the identity. Never normalize partial lines. */
static inline unsigned leo_ax059_length(const char *s)
{unsigned n=0;while(s[n])++n;return n;}
static inline int leo_ax059_equal(const char *p,const char *s,unsigned n)
{for(unsigned i=0;i<n;i++)if(p[i]!=s[i])return 0;return 1;}
static inline int leo_ax059_hex(unsigned char c)
{return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}
static inline int leo_ax059_identity(const char *text,unsigned bytes,
 uint32_t truncated,uint32_t gap,uint32_t *hash)
{
 static const char missing[]="|warning| AX SpringBoardServer: Error: Error Domain=AXIPC Code=";
 static const char ipc[]="|AXIPC|warning| Could not send message (com.apple.accessibility.AXSpringBoardServer). Port:";
 const char *marker=0,*end=0;unsigned prefix,marker_n,skip_start=0,skip_end=0,found=0;
 if(!text||!hash||truncated||gap||bytes>=384U||text[bytes])return 0;
 prefix=sizeof(missing)-1U;
 if(bytes>=prefix&&leo_ax059_equal(text,missing,prefix)){marker="UserInfo=0x";end=" {";}
 else{
  prefix=sizeof(ipc)-1U;
  if(bytes>=prefix&&leo_ax059_equal(text,ipc,prefix)){marker="<AXIPCMessage: 0x";end=">. Client port:";}
 }
 if(!marker)return 0;
 marker_n=leo_ax059_length(marker);
 for(unsigned i=prefix;i+marker_n<=bytes;i++){
  if(!leo_ax059_equal(text+i,marker,marker_n))continue;
  unsigned start=i+marker_n,stop=start;
  while(stop<bytes&&leo_ax059_hex((unsigned char)text[stop]))stop++;
  unsigned digits=stop-start,end_n=leo_ax059_length(end);
  if(!digits||digits>8U||stop+end_n>bytes||!leo_ax059_equal(text+stop,end,end_n))return 0;
  if(found++)return 0; /* ambiguous multiple pointers retain original identity */
  skip_start=start;skip_end=stop;
 }
 if(found!=1U)return 0;
 uint32_t h=2166136261U;
 for(unsigned i=0;i<bytes;i++){
  if(i==skip_start){h=(h^(unsigned)'*')*16777619U;i=skip_end-1U;}
  else h=(h^(unsigned char)text[i])*16777619U;
 }
 *hash=h;return 1;
}
#endif
