/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
#ifndef IOS7_LEO_TOUCH_OBSERVATION_H
#define IOS7_LEO_TOUCH_OBSERVATION_H
#include <stdint.h>
#define IOS7_LEO_TOUCH_OBSERVATION_VERSION 2U
#define IOS7_LEO_TOUCH_OBSERVATION_BYTES 192U
#define LEO_TOUCH_OBS_PORT_PRESENT 1U
#define LEO_TOUCH_OBS_COUNTER_SATURATED 2U
#define LEO_TOUCH_OBS_NOTIFY_RESULT_VALID 4U
#define LEO_TOUCH_UP_NOTIFY_SELECTED 1U
#define LEO_TOUCH_UP_NOTIFY_PORT 2U
#define LEO_TOUCH_UP_NOTIFY_OK 4U
#define LEO_TOUCH_UP_NOTIFY_BUSY 8U
#define LEO_TOUCH_UP_NOTIFY_ERROR 16U
#define LEO_TOUCH_UP_WAS_EMPTY 32U
#define LEO_TOUCH_UP_EMPTIED_DURING 64U
typedef struct LeoTouchObservation {
 /* The original112-byte prefix retains its meanings, with version=2. */
 uint32_t version,known,init_valid,init_ready;
 int32_t init_result;
 uint32_t init_stage,id_word,published,client_pid,opened,map_calls,notification_calls;
 uint32_t real_reports,enqueued,down_events,move_events,up_events,queue_failures;
 uint32_t reader_progress,reader_head,writer_tail,pending,last_sequence,last_x,last_y,last_down;
 uint32_t transport_errors,loss;
 uint32_t queue_epoch,last_up_epoch,last_up_enqueued,last_up_tail,last_up_sequence,last_up_x,last_up_y,last_up_notify_flags;
 uint64_t last_up_timestamp,service_sender;
 uint32_t notify_calls,notify_ok,notify_busy,notify_errors,notify_no_port,last_notify_result,last_notify_tail,observation_flags;
} LeoTouchObservation;
typedef char LeoTouchObservation192[(sizeof(LeoTouchObservation)==IOS7_LEO_TOUCH_OBSERVATION_BYTES)?1:-1];
typedef char LeoTouchObservationPrefix112[(__builtin_offsetof(LeoTouchObservation,queue_epoch)==112)?1:-1];
typedef char LeoTouchObservationTime144[(__builtin_offsetof(LeoTouchObservation,last_up_timestamp)==144)?1:-1];
/* Bounded RAM copy only: zero and set version=2 before calling. A version1
 * caller is rejected before any copy. No pointer, MMIO, allocation or wait.
 * queue_epoch increments on every queue discard/open-close, saturating;
 * service_sender is the real registry sender used by the event serializer.
 * last_up_* describes the last SUCCESSFULLY ENQUEUED real Up, not the latest
 * raw report. timestamp is the actual packet's mach_absolute_time domain.
 * Poll publishes head/tail even when no input arrives or a read fails.
 * notify_calls counts selected notification paths, including absent ports;
 * OK/BUSY/ERROR count actual Mach sends only. BUSY is MACH_SEND_TIMED_OUT,
 * meaning an already queued notification, not proof of user consumption.
 * Result is UINT32_MAX when no real send occurred, and valid only if flag4.
 * observation_flags reports installed port and any saturated counter; use
 * saturated values as lower bounds, never a stable provenance equality.
 * Delayed FIFO removal through Up can be established only with the same
 * service_sender/queue_epoch==last_up_epoch, enqueued==last_up_enqueued,
 * opened, !pending and reader_head==last_up_tail, with no saturation. This
 * does NOT establish IOHID parsing, BackBoard acceptance or a scene change.
 * Existing last_sequence/last_x/y/down describe RAW transport state and may
 * advance while idle; they are not the fixed Up provenance.
 * loss remains a lower bound on missed copies, UINT32_MAX means unknown. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t ios7leo_touch_observe(LeoTouchObservation *out);
#ifdef __cplusplus
}
#endif
#endif
