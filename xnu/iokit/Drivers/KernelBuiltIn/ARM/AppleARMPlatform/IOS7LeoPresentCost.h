#ifndef IOS7_LEO_PRESENT_COST_H
#define IOS7_LEO_PRESENT_COST_H
#include <stdint.h>
enum { LEO_PRESENT_COST_CONVERSION=1U,LEO_PRESENT_COST_POLL=2U,LEO_PRESENT_COST_ROW_SAMPLE=4U };
/* Wall-clock mach_absolute_time ticks, including preemption, not CPU cycles.
 * Conversion/poll durations belong to last_completed_sequence only.
 * Row sample is row0 every128 attempts; maximum covers sampled rows only. */
typedef struct LeoPresentCostObservation {
 uint32_t version,known,attempts,completed;
 uint32_t last_conversion_rows,row_samples,reserved0,reserved1;
 uint64_t last_completed_sequence,last_conversion_ticks,last_poll_ticks;
 uint64_t total_conversion_ticks,total_poll_ticks,last_row_sample_ticks,max_row_sample_ticks;
 uint64_t last_row_sample_sequence;
} LeoPresentCostObservation;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_present_cost_observe(LeoPresentCostObservation *out);
#ifdef __cplusplus
}
#endif
#endif
