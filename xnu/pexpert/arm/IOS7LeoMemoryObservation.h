#ifndef IOS7_LEO_MEMORY_OBSERVATION_H
#define IOS7_LEO_MEMORY_OBSERVATION_H

#include <stdint.h>

#define IOS7_LEO_MEMORY_OBS_VERSION 1U
#define IOS7_LEO_MEMORY_MAX_BANKS 8U

enum {
    LEO_MEMORY_STATUS_UNKNOWN = 0U,
    LEO_MEMORY_STATUS_OK = 1U,
    LEO_MEMORY_STATUS_BAD_HANDOFF = 2U,
    LEO_MEMORY_STATUS_MAP_FAILED = 3U,
    LEO_MEMORY_STATUS_PARSE_FAILED = 4U,
    LEO_MEMORY_STATUS_COUNTERS_PARTIAL = 5U
};

enum {
    LEO_MEMORY_KNOWN_ATAG = 1U << 0,
    LEO_MEMORY_KNOWN_VM_COUNTERS = 1U << 1,
    /* Admission by the exact ordinary-LEO board profile; this is not a
     * destructive probe or a claim that every admitted byte ran on hardware. */
    LEO_MEMORY_KNOWN_BOARD_PROFILE = 1U << 2
};

typedef struct LeoMemoryObservation {
    uint32_t version;
    uint32_t known;
    uint32_t status;
    uint32_t sequence;
    uint32_t atag_base;
    uint32_t atag_end;
    uint32_t atag_bytes;
    uint32_t bank_count;
    struct {
        uint32_t base;
        uint32_t bytes;
    } banks[IOS7_LEO_MEMORY_MAX_BANKS];
    uint32_t selected_base;
    uint32_t selected_bytes;
    uint64_t managed_bytes;
    uint32_t free_pages;
    uint32_t free_reserved;
    uint32_t free_target;
    uint64_t pageins;
    uint64_t pageouts;
    uint64_t faults;
} LeoMemoryObservation;

#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_memory_observe(LeoMemoryObservation *out);
#ifdef __cplusplus
}
#endif

#endif
