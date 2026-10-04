#ifndef IOS7_LEO_MDP_FAULT_H
#define IOS7_LEO_MDP_FAULT_H
#include <stdint.h>
#define IOS7_LEO_MDP_OBS_VERSION 1U
enum {
 LEO_MDP_KNOWN_LIVE=1U, LEO_MDP_KNOWN_FIRST_FAILURE=2U,
 LEO_MDP_KNOWN_FIRST_RECOVERY=4U
};
/* Separate diagnostic ABI. Existing LeoDisplayObservation remains unchanged.
 * One first hardware/presentation failure and one first actual late-DONE
 * acknowledgement are retained; live counters are observations only.
 * A late acknowledgement is NOT successful completion of the failed call. */
typedef struct LeoMDPFaultObservation {
 uint32_t version,known;
 uint32_t first_valid,first_phase,first_status,first_config,first_size;
 uint32_t first_address,first_stride,first_xy,first_polls,first_pending;
 uint32_t first_attempts,first_kicks,first_completed;
 uint64_t first_elapsed,first_interval,first_sequence;
 uint32_t pending,latched,late_pending_count,late_ack_count,pending_rejects,last_ack_status;
 uint32_t first_recovery_valid,first_recovery_status,first_recovery_pending_count;
 uint32_t first_recovery_kicks,first_recovery_completed;
 uint64_t first_recovery_sequence;
} LeoMDPFaultObservation;
/* Caller zeroes, sets version=1, invokes in an ordinary worker outside storage
 * locks. Bounded IRQ-masked RAM copy only: no MMIO, allocation, formatting,
 * logging, new thread or storage access. Fields with first_valid/recovery_valid
 * unset are unknown, not proof that no hardware failure occurred. */
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_mdp_observe(LeoMDPFaultObservation *out);
#ifdef __cplusplus
}
#endif
#endif
