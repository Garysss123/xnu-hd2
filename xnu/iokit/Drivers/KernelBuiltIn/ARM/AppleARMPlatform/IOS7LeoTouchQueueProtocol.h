/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
#ifndef IOS7_LEO_TOUCH_QUEUE_PROTOCOL_H
#define IOS7_LEO_TOUCH_QUEUE_PROTOCOL_H
#include <stdint.h>
#include "IOS7LeoTouchObservation.h"
typedef struct LeoTouchQueueHooks {
 void *cookie;
 void (*barrier)(void *cookie);
 void (*copy)(void *cookie,void *destination,const void *source,uint32_t bytes);
} LeoTouchQueueHooks;
/* Original IODataQueue 4096-byte SPSC ring, 4-byte entry header +204-byte
 * original native packet. The service lock serializes the one producer.
 * 1: acquire the consumer head before reusing a slot.
 * 2: publish payload/entry size before tail (release).
 * 3: order tail publication before the fresh head read (full store/load).
 * With the consumer's matching ordered head-publication/empty-check, the
 * third fence prevents both ends observing the old empty frontier in the
 * drain-last-Move/enqueue-Up race. This does not establish the particular
 * native consumer's fences or prove that race occurred on a boot.
 * Layout, wrap marker, strict full test and notification
 * conditions remain the original protocol; notify flags describe selection,
 * never an invented successful notification or a synthetic input. */
template <typename QueueWord>
static int leo_touch_queue_enqueue(volatile QueueWord *head,volatile QueueWord *tail,
 uint32_t capacity,unsigned char *queue,const void *data,uint32_t bytes,
 const LeoTouchQueueHooks *hooks,uint32_t *notificationFlags)
{
 typedef char LeoQueueWord32[(sizeof(QueueWord)==4)?1:-1];(void)sizeof(LeoQueueWord32);
 if(notificationFlags)*notificationFlags=0;
 if(!head || !tail || !queue || !data || !hooks || !hooks->barrier || !hooks->copy ||
  !notificationFlags || capacity!=4096U || bytes!=204U)return 0;
 uint32_t h=*head,t=*tail,entry=bytes+4U;
 if(h>capacity || t>capacity || (h&3U) || (t&3U))return 0;
 hooks->barrier(hooks->cookie);
 uint32_t position,newTail;
 if(t>=h){
  if(t+entry<=capacity){position=t;newTail=t+entry;}
  else if(h>entry){
   position=0;newTail=entry;
   if(capacity-t>=4U)*(QueueWord *)(queue+t)=bytes;
  }else return 0;
 }else{
  if(h-t<=entry)return 0;
  position=t;newTail=t+entry;
 }
 *(QueueWord *)(queue+position)=bytes;
 hooks->copy(hooks->cookie,queue+position+4U,data,bytes);
 hooks->barrier(hooks->cookie);
 *tail=newTail;
 hooks->barrier(hooks->cookie);
 uint32_t freshHead=*head;
 if(h==t)*notificationFlags|=LEO_TOUCH_UP_WAS_EMPTY;
 if(freshHead==t)*notificationFlags|=LEO_TOUCH_UP_EMPTIED_DURING;
 if(*notificationFlags)*notificationFlags|=LEO_TOUCH_UP_NOTIFY_SELECTED;
 return 1;
}
#endif
