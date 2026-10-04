/* Owned QSD8250 XNU timer API. Register provenance is in CONTRACT.md. */
#ifndef IOS7LAB_LEO_QSD8250_TIMER_H
#define IOS7LAB_LEO_QSD8250_TIMER_H
#include <mach/mach_types.h>

#define LEO_GPT_HZ 32768U
#define LEO_GPT_IRQ 7U
#define LEO_GPT_WRITE_DELAY_TICKS 9U
#define LEO_GPT_MIN_MATCH_TICKS (LEO_GPT_WRITE_DELAY_TICKS+4U)

struct LeoTimerSnapshot {
    uint32_t version, valid_flags, count, match, enable, irq_status;
    uint32_t irq_count, last_irq_counter, last_vector, spurious_irq_count;
    uint32_t frequency_hz, minimum_match_ticks;
    uint64_t extended_ticks;
};
typedef char LeoTimerSnapshot_size[(sizeof(struct LeoTimerSnapshot)==56)?1:-1];

/* These six signatures exactly match SocDeviceDispatch in arm/protos.h. */
void Leo_interrupt_init(void);
void Leo_timebase_init(void);
void Leo_handle_interrupt(void *context);
uint64_t Leo_get_timebase(void);
uint64_t Leo_timer_value(void);
void Leo_timer_enabled(int enable);
void Leo_timer_snapshot(struct LeoTimerSnapshot *out);
extern uint64_t clock_decrementer;
#endif
