#ifndef IOS7_LEO_SWITCH_OBSERVATION_H
#define IOS7_LEO_SWITCH_OBSERVATION_H
#include <stdint.h>
typedef struct {uint32_t version,known,calls3,calls4,calls5,paused,restored,
 waits,depresses,handoffs,last_tid_low,last_tid_high;} LeoSwitchObservation;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_switch_observe(LeoSwitchObservation *out);
#ifdef __cplusplus
}
#endif
#endif
