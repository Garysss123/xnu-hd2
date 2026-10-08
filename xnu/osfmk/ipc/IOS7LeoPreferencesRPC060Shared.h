#ifndef IOS7LEO_PREFERENCES_RPC060_SHARED_H
#define IOS7LEO_PREFERENCES_RPC060_SHARED_H
#include <stdint.h>
enum {LEO_RPC060_ENTRY=1,LEO_RPC060_HEADER=2,LEO_RPC060_COPYIN=3,
 LEO_RPC060_SEND=4,LEO_RPC060_WAIT=5,LEO_RPC060_RECEIVE=6,LEO_RPC060_DONE=7};
typedef struct LeoPrefsRPC060 {
 uint64_t tid,entry_absolute,last_absolute,message_address,receive_address;
 uint32_t known,pid,pidversion,call,stage,option,send_size,receive_limit,receive_name,timeout;
 uint32_t header_valid,header_size,bits,remote_name,local_name;
 int32_t message_id;
 uint32_t active_receive,receive_header_valid,received_size,receive_state,result;
 int32_t received_id;
} LeoPrefsRPC060;
uint32_t ios7leo_prefsrpc060_observe(LeoPrefsRPC060 *);
void ios7leo_prefsrpc060_stop(void);
#define LEO_RPC060_FORMAT "PREFRPC060 S=%u KN=%u PID=%u VER=%u TID=%llx CALL=%u STAGE=%u OPT=%x SND_RCV=%x/%x RCV=%x TO=%x HDR=%u:%d:%x:%x:%x:%x RHDR=%u:%d:%x MR=%x/%x TICKS=%llu/%llu ADDR=%llx/%llx ACTIVE=%u last-call-only;names-not-endpoint\n"
#endif
