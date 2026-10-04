#ifndef IOS7_LEO_TOUCH_OBSERVATION_H
#define IOS7_LEO_TOUCH_OBSERVATION_H
#include <stdint.h>
#define IOS7_LEO_TOUCH_OBSERVATION_VERSION 1U
typedef struct LeoTouchObservation {
 uint32_t version,known,init_valid,init_ready;
 int32_t init_result;
 uint32_t init_stage,id_word,published,client_pid,opened,map_calls,notification_calls;
 uint32_t real_reports,enqueued,down_events,move_events,up_events,queue_failures;
 uint32_t reader_progress,reader_head,writer_tail,pending,last_sequence,last_x,last_y,last_down;
 uint32_t transport_errors,loss;
} LeoTouchObservation;
typedef char LeoTouchObservation112[(sizeof(LeoTouchObservation)==112)?1:-1];
/* Call in the existing ordinary logger worker. Zero and set version=1 first.
 * Returns1 for a bounded RAM snapshot,0 for unavailable/busy/bad version.
 * No object pointer, MMIO, allocation, formatting, wait or new worker is used.
 * loss is a lower bound on missed observation copies; UINT32_MAX is unknown.
 * init_valid means a real init call returned, including a negative result.
 * published/open/map/notification are instantaneous protocol observations;
 * enqueue/head progression never establishes application gesture handling. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t ios7leo_touch_observe(LeoTouchObservation *out);
#ifdef __cplusplus
}
#endif
#endif
