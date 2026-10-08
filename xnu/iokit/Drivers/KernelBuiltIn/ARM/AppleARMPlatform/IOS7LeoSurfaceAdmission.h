#ifndef IOS7_LEO_SURFACE_ADMISSION_H
#define IOS7_LEO_SURFACE_ADMISSION_H
#include <stdint.h>
/* User-requested runtime reserve floor:5MiB, or higher existing VM watermarks.
 * Boot admission,16MiB totalcap,64slots and half remaining headroom stay separate. */
static inline uint64_t leo_surface_runtime_guard(uint32_t reserved,uint32_t target,uint32_t budget)
{uint64_t kernel=(uint64_t)reserved+target;(void)budget;return kernel>1280U?kernel:1280U;}
static inline int leo_surface_runtime_room(uint64_t managed,uint32_t freePages,
 uint32_t reserved,uint32_t target,uint32_t budget,uint32_t primary,uint32_t heap,uint32_t additional)
{
 uint64_t guard=leo_surface_runtime_guard(reserved,target,budget);
 uint64_t total=(uint64_t)primary+4096U+heap+additional+65536U;
 uint64_t addPages=((uint64_t)additional+4095U)>>12;
 return managed && budget && total<=budget && ((total+4095U)>>12)<=(managed>>3) &&
        freePages>=guard && addPages<=((uint64_t)freePages-guard)/2U;
}
typedef struct {
 uint32_t version,known,denials,first_phase,below_old_admitted;
 uint32_t first_free,first_guard,first_charge,first_width,first_height;
 uint32_t current_free,current_guard,old_guard,budget,heap,primary;
} LeoSurfaceAdmissionObservation;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_surface_admission_observe(LeoSurfaceAdmissionObservation *out);
#ifdef __cplusplus
}
#endif
#endif
