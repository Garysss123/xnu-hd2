#ifndef IOS7_LEO_SD_FAULT_H
#define IOS7_LEO_SD_FAULT_H
#include <stdint.h>
typedef struct LeoSDFault {
 uint32_t version,valid,operation,phase;
 int32_t result;
 uint32_t command,status,words,write_completed;
 uint64_t sector,when_ns;
} LeoSDFault;
#ifdef __cplusplus
extern "C" {
#endif
void ios7leo_sd_fault_publish(const LeoSDFault *);
#ifdef __cplusplus
}
#endif
#endif
