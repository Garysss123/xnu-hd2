#ifndef LEO_TOUCH_RELEASE_WATCH_H
#define LEO_TOUCH_RELEASE_WATCH_H
#include "IOS7LeoTouchObservation.h"

/* Existing logger worker only. The anchor is when that worker observes an Up,
 * not its hardware timestamp. A newer release supersedes an unfinished pair.
 * At most four delayed samples are emitted for the entire boot. */
typedef struct LeoTouchReleaseWatch {
 uint64_t sender,up_timestamp,anchor_ns;
 uint32_t epoch,up_enqueued,up_sequence,up_tail,stage,emitted,active;
} LeoTouchReleaseWatch;
typedef struct LeoTouchReleaseSample {
 uint64_t observed_up_ns,now_ns,elapsed_ns;
 uint32_t delay_seconds,ordinal,queue_removed;
} LeoTouchReleaseSample;

static inline int leo_touch_release_sample(LeoTouchReleaseWatch *watch,
 const LeoTouchObservation *d,uint64_t now,LeoTouchReleaseSample *sample)
{
 if(!watch||!d||!sample||!now||d->version!=IOS7_LEO_TOUCH_OBSERVATION_VERSION||!d->known)return 0;
 if(!d->opened||!d->service_sender||!d->last_up_timestamp||!d->last_up_enqueued||
    !d->queue_epoch||d->last_up_epoch!=d->queue_epoch){watch->active=0;return 0;}
 if(!watch->active||watch->sender!=d->service_sender||watch->epoch!=d->queue_epoch||
    watch->up_timestamp!=d->last_up_timestamp||watch->up_sequence!=d->last_up_sequence||
    watch->up_enqueued!=d->last_up_enqueued||watch->up_tail!=d->last_up_tail){
  watch->sender=d->service_sender;watch->epoch=d->queue_epoch;
  watch->up_timestamp=d->last_up_timestamp;watch->up_sequence=d->last_up_sequence;
  watch->up_enqueued=d->last_up_enqueued;watch->up_tail=d->last_up_tail;
  watch->anchor_ns=now;watch->stage=0;watch->active=1;return 0;
 }
 if(watch->emitted>=4U||watch->stage>=2U||now<watch->anchor_ns)return 0;
 uint32_t delay=watch->stage==0?5U:30U;
 uint64_t elapsed=now-watch->anchor_ns;
 if(elapsed<(uint64_t)delay*1000000000ULL)return 0;
 sample->observed_up_ns=watch->anchor_ns;sample->now_ns=now;
 sample->elapsed_ns=elapsed;sample->delay_seconds=delay;
 sample->ordinal=++watch->emitted;watch->stage++;
 /* A same-generation empty FIFO ending exactly at this Up's published tail
  * proves removal through the Up, not BackBoard dispatch or gesture acceptance.
  * A newer enqueue/pending packet or a saturated counter makes it unknown. */
 sample->queue_removed=(d->observation_flags&LEO_TOUCH_OBS_COUNTER_SATURATED)==0&&
  d->enqueued==watch->up_enqueued&&!d->pending&&
  d->reader_head==watch->up_tail&&d->writer_tail==watch->up_tail;
 return 1;
}
#endif
