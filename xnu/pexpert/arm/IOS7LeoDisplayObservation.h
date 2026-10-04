#ifndef IOS7_LEO_DISPLAY_OBSERVATION_H
#define IOS7_LEO_DISPLAY_OBSERVATION_H
#include <stdint.h>
enum {
 LEO_OBS_PE_COUNTERS=1U, LEO_OBS_SCANOUT_SAMPLES=2U,
 LEO_OBS_PRIMARY_SAMPLES=4U, LEO_OBS_PRIMARY_LOCK_BUSY=8U
};
typedef struct LeoDisplayObservation {
 uint32_t version,known;
 uint32_t present_attempts,conversion_rows,last_conversion_rows,completed,failures,last_failure;
 uint32_t graphics_owner,fatal_active;
 uint32_t normal_refresh_requests,normal_owner_blocks,normal_pending_skips,normal_kicks,graphics_kicks,last_status;
 uint32_t normal_kicks_at_graphics_claim,normal_kicks_after_owner;
 uint64_t completed_sequence_before,completed_sequence_after;
 uint32_t scanout_sample_hash,scanout_nonzero_rgb,scanout_samples;
 uint32_t primary_sample_hash,primary_nonzero_rgb,primary_nonzero_alpha,primary_samples;
 uint32_t primary_bytes,primary_seed_before,primary_seed_after,active_surface;
} LeoDisplayObservation;
/* Caller zeroes the record, sets version=1 and calls BOTH APIs only in an
 * ordinary worker context outside SD/spool locks. Never an IRQ/fatal producer.
 * The two planes are separate bounded observations, not an atomic whole-frame.
 * Fixed32x32 grid: x=column*479/31,y=row*799/31,1024 samples per plane.
 * Primary-lock busy means samples unknown, not absent pixels. Hash covers only
 * these samples; zero/equality does not prove the rest of the image is blank.
 * Seed/sequence equality only means these metadata did not change; it does
 * not establish that a userspace writer obeyed seed/fence serialization.
 * A kick is not DMA completion; completion counters require fresh actual DONE.
 * Counters are uint32 observations and can wrap; no reset or behavior control.
 */
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_scanout_observe(LeoDisplayObservation *out);
void ios7leo_primary_observe(LeoDisplayObservation *out);
#ifdef __cplusplus
}
#endif
#endif
