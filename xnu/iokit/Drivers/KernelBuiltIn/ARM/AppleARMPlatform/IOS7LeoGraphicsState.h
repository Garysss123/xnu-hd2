#ifndef IOS7LEO_GRAPHICS_STATE_H
#define IOS7LEO_GRAPHICS_STATE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LEO_GRAPHICS_STATE_VERSION 2U
#define LEO_GRAPHICS_EVENT_CAPACITY 16U
enum {
 LEO_GRAPHICS_EVENT_METHOD=1, LEO_GRAPHICS_EVENT_PORT=2,
 LEO_GRAPHICS_EVENT_SEND_FAILURE=3, LEO_GRAPHICS_EVENT_STALE_SEND=4,
 LEO_GRAPHICS_EVENT_CLOSE=5, LEO_GRAPHICS_EVENT_COPY_SEND_FAILURE=6,
 LEO_GRAPHICS_EVENT_TIMER_FAILURE=7
};
enum {
 LEO_GRAPHICS_REASON_NONE=0, LEO_GRAPHICS_REASON_ARGUMENT=0x100,
 LEO_GRAPHICS_REASON_DESCRIPTOR=0x101, LEO_GRAPHICS_REASON_CLOSED=0x102,
 LEO_GRAPHICS_REASON_WAIT=0x103, LEO_GRAPHICS_REASON_TIMER=0x104,
 LEO_GRAPHICS_REASON_UNSUPPORTED=0x105, LEO_GRAPHICS_REASON_BUSY=0x106,
 LEO_GRAPHICS_REASON_PORT_COPY=0x107
};
/* Aggregated counters; configuration fields are the last observed client,
 * identified by last_client/owner_pid. They are not hardware-vblank or UI proof. */
typedef struct LeoGraphicsState {
 uint32_t version,known,loss,unknown,clients,last_client,owner_pid;
 /* Historical latest-open identity, not proof that this client is current,
  * primary, alive, or the owner of a later submission. */
 uint32_t latest_open_client,latest_open_pid;
 /* Supported caller prerequisites. An entry sets prereq_last_result to
  * UINT32_MAX until the same synchronous externalMethod call returns. */
 uint32_t selector3_calls,selector3_successes,selector8_calls,selector8_successes;
 uint32_t selector18_calls,selector18_successes;
 uint32_t prereq_last_client,prereq_last_pid,prereq_last_selector,prereq_last_result;
 uint32_t selector4_calls,selector4_rejects,selector5_calls,selector5_rejects;
 uint32_t selector5_backend_calls,selector5_completed,selector6_calls,selector6_rejects;
 uint32_t selector9_calls,selector9_rejects,port_calls,port_rejects;
 /* registerNotificationPort pipeline. port_calls remains gated-result count.
  * With unsaturated counters and loss==0, entries>(preflight+gate_entries)
  * identifies wrapper work before gate entry; gate_entries>port_calls identifies
  * gated work without a result; (preflight+port_calls)>port_returns identifies
  * the final synchronous return gap.
  * Latest entry identity is historical and concurrency-qualified by loss. */
 uint32_t port_entries,port_preflight_rejects,port_gate_entries,port_returns;
 uint32_t port_entry_client,port_entry_pid,port_entry_type,port_entry_valid;
 uint32_t port_last_return;
 uint32_t send_calls,send_success,send_failures,copy_send_failures,stale_completions;
 uint32_t last_selector,last_result,last_reason,last_transaction_lo,last_transaction_hi;
 uint32_t last_surface,last_mismatch,last_wait_lo,last_wait_hi;
 uint32_t requested,enabled,closed,port_present,callback_present;
 uint32_t generation_lo,generation_hi,last_send_status,last_timer_status;
 uint32_t events_published,events_taken;
 uint32_t selector4_result,selector5_result,selector6_result,selector9_result;
 uint32_t selector4_reason,selector5_reason,selector6_reason,selector9_reason;
} LeoGraphicsState;
/* First sixteen control/submission/failure records, then qualified loss.
 * No per-tick successful-send event. All values were already in kernel memory. */
typedef struct LeoGraphicsEvent {
 uint32_t sequence,kind,client,owner_pid,selector,result,reason;
 uint32_t transaction_lo,transaction_hi,surface,mismatch;
 uint32_t generation_lo,generation_hi,requested,enabled,closed;
 uint32_t port_present,callback_present,send_count_lo,send_count_hi;
} LeoGraphicsEvent;
/* Normal worker only. Try-only guard, no allocation, formatting, clocks or I/O.
 * Return zero means unavailable/busy; it never implies no activity. */
uint32_t ios7leo_graphics_snapshot(LeoGraphicsState *out);
uint32_t ios7leo_graphics_event_take(LeoGraphicsEvent *out,uint32_t *lost);
#ifdef __cplusplus
}
#endif
#endif
