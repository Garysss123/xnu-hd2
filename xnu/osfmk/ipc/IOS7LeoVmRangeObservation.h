#ifndef IOS7_LEO_VM_RANGE_OBSERVATION_H
#define IOS7_LEO_VM_RANGE_OBSERVATION_H
#include <stdint.h>
typedef struct {uint32_t version,known,deallocate_calls,deallocate_ok,
 protect_calls,protect_ok,rejected_high,wrong_task,backend_errors,
 nonzero_deallocate_ok,last_address,last_size,last_protection,last_result;
} LeoVmRangeObservation;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_vm_range_observe(LeoVmRangeObservation *out);
#ifdef __cplusplus
}
#endif
#endif
