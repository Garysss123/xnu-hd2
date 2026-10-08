#ifndef IOS7LEO_QUEUED_STATE063_SHARED_H
#define IOS7LEO_QUEUED_STATE063_SHARED_H
#include <stdint.h>
typedef struct LeoQueuedState063 {
 uint32_t attempts,eligible,removed,requeued,success,backend_error,remove_failed;
 uint64_t caller_tid,target_tid;
 uint32_t last_state,last_priority,last_base_priority,last_count,last_result;
} LeoQueuedState063;
uint32_t ios7leo_queuedstate063_observe(LeoQueuedState063 *);
#define LEO_QUEUED063_FORMAT "PREFQSTATE065 S=%u ATT_ELIG_REMOVE_REQUEUE=%u/%u/%u/%u OK_ERR_REMOVEFAIL=%u/%u/%u CALLER_TARGET=%llx/%llx STATE_PRI_BASE_COUNT_RESULT=%x/%u/%u/%u/%x real-register-copy;queue-restored;not-launch-proof\n"
#endif
