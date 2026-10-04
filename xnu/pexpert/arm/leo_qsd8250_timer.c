/* Owned QSD8250 single-core XNU IRQ/timebase adaptation.
 * Linux QSD8x50 register protocol and the HTC Leo VIC/GPT register configuration
 * are referenced, not compiled/imported. No periodic counter fabrication.
 */
#include <mach/mach_types.h>
#include <mach/arm/thread_status.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm/protos.h>
#include <machine/machine_routines.h>
#include <kern/debug.h>
#include "leo_qsd8250_timer.h"

#if defined(BOARD_CONFIG_QSD8250_LEO)
#define VIC_PA 0xac000000U
#define CSR_PA 0xac100000U
#define IO_BYTES 4096U
#define GPT_MASK (1U<<LEO_GPT_IRQ)
#define VIC_SELECT0 0x000U
#define VIC_SELECT1 0x004U
#define VIC_ENCLEAR0 0x020U
#define VIC_ENCLEAR1 0x024U
#define VIC_ENSET0 0x030U
#define VIC_TYPE0 0x040U
#define VIC_TYPE1 0x044U
#define VIC_POLARITY0 0x050U
#define VIC_POLARITY1 0x054U
#define VIC_MASTER 0x068U
#define VIC_CONFIG 0x06cU
#define VIC_STATUS0 0x080U
#define VIC_CLEAR0 0x0b0U
#define VIC_CLEAR1 0x0b4U
#define VIC_VECTOR 0x0d0U
#define VIC_PENDING_VECTOR 0x0d4U
#define GPT_MATCH 0x000U
#define GPT_COUNT 0x004U
#define GPT_ENABLE 0x008U
#define GPT_CLEAR 0x00cU
#define LEO_BOARD_DIVIDER 0x020U
#define MAX_MATCH_TICKS 0x7fffffffU

extern void rtc_configure(uint64_t hz);
extern void rtclock_intr(arm_saved_state_t *regs);
extern void ios7leo_timer_observe(uint32_t raw_gpt_ticks, uint32_t actual_irq7_count);

uint64_t clock_decrementer;
static vm_offset_t vic, csr;
static uint32_t controller_ready, timebase_ready, counter_observed, armed;
static uint32_t last_count, irq_count, last_irq_counter, last_vector;
static uint32_t spurious_irq_count;
static uint64_t extended_ticks;

static uint32_t read_reg(vm_offset_t base, uint32_t offset)
{
    return *(volatile uint32_t *)(base+offset);
}
static void write_reg(vm_offset_t base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base+offset)=value;
}
static void drain(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* UP only: caller holds IRQ masked while extending the real 32-bit counter.
 * Unsigned delta includes a single rollover. Frequent timer IRQs/reads are
 * required; more than one unattended 36.4-hour GPT rollover is unknowable. */
static uint64_t sample_counter_locked(void)
{
    uint32_t now=read_reg(csr,GPT_COUNT);
    extended_ticks+=(uint32_t)(now-last_count);
    last_count=now;
    return extended_ticks;
}

void Leo_interrupt_init(void)
{
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    if(controller_ready) { ml_set_interrupts_enabled(was_enabled); return; }
    vic=ml_io_map(VIC_PA,IO_BYTES);
    csr=ml_io_map(CSR_PA,IO_BYTES);
    if(!vic || !csr) panic("Leo VIC/CSR IO mapping failed");
    write_reg(vic,VIC_MASTER,0);
    write_reg(vic,VIC_SELECT0,0); write_reg(vic,VIC_SELECT1,0);
    write_reg(vic,VIC_ENCLEAR0,0xffffffffU);
    write_reg(vic,VIC_ENCLEAR1,0xffffffffU);
    write_reg(vic,VIC_CLEAR0,0xffffffffU);
    write_reg(vic,VIC_CLEAR1,0xffffffffU);
    write_reg(vic,VIC_TYPE0,GPT_MASK); write_reg(vic,VIC_TYPE1,0);
    write_reg(vic,VIC_POLARITY0,0); write_reg(vic,VIC_POLARITY1,0);
    write_reg(vic,VIC_CONFIG,0); /* Linux/proven Leo: non-vectored mode. */
    drain();
    controller_ready=1;
    write_reg(vic,VIC_MASTER,1); /* IRQ master only; FIQ remains disabled. */
    drain();
    ml_set_interrupts_enabled(was_enabled);
}

void Leo_timebase_init(void)
{
    uint32_t before, after, i;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    if(!controller_ready) panic("Leo timebase before VIC initialization");
    if(timebase_ready) { ml_set_interrupts_enabled(was_enabled); return; }
    write_reg(vic,VIC_ENCLEAR0,GPT_MASK);
    /* Same HTC Leo board write as HTC Linux register references. GPT remains 32768 Hz;
     * CSR+20 is also the board DGT divider, not a fabricated GPT multiplier. */
    write_reg(csr,LEO_BOARD_DIVIDER,3);
    write_reg(csr,GPT_ENABLE,0);
    write_reg(csr,GPT_CLEAR,1);
    write_reg(csr,GPT_COUNT,0);
    write_reg(csr,GPT_MATCH,0xffffffffU);
    drain();
    write_reg(vic,VIC_CLEAR0,GPT_MASK);
    write_reg(csr,GPT_ENABLE,1); /* Free-running; NEVER clear on match. */
    drain();
    before=read_reg(csr,GPT_COUNT);
    after=before;
    for(i=0;i<65536U;i++) {
        after=read_reg(csr,GPT_COUNT);
        if(after!=before) break;
    }
    if(after==before) panic("Leo GPT counter did not advance");
    counter_observed=1;
    last_count=after; extended_ticks=0;
    timebase_ready=1;
    clock_decrementer=32; /* Actual 32768-Hz ticks, about 0.977 ms. */
    gPEClockFrequencyInfo.timebase_frequency_hz=LEO_GPT_HZ;
    gPEClockFrequencyInfo.dec_clock_rate_hz=LEO_GPT_HZ;
    rtc_configure(LEO_GPT_HZ);
    Leo_timer_enabled(TRUE);
    /* Normal XNU boot owns CPU IRQ enable; no IRQ wait or standalone hold. */
    ml_set_interrupts_enabled(was_enabled);
}

void Leo_timer_enabled(int enable)
{
    uint32_t ticks, attempt, now, match, after;
    uint64_t requested;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    if(!timebase_ready) panic("Leo timer before timebase initialization");
    write_reg(vic,VIC_ENCLEAR0,GPT_MASK);
    armed=0;
    write_reg(csr,GPT_MATCH,0); /* Disarm compare, preserve running COUNT. */
    write_reg(vic,VIC_CLEAR0,GPT_MASK);
    drain();
    requested=clock_decrementer;
    if(!enable || requested==~0ULL) {
        ml_set_interrupts_enabled(was_enabled); return;
    }
    ticks=requested>MAX_MATCH_TICKS?MAX_MATCH_TICKS:(uint32_t)requested;
    if(ticks<LEO_GPT_MIN_MATCH_TICKS) ticks=LEO_GPT_MIN_MATCH_TICKS;
    for(attempt=0;attempt<4U;attempt++) {
        /* Clear OLD edge before MATCH. A new edge after MATCH must remain
         * latched, even if it occurs before ENSET unmasks the IRQ. */
        write_reg(vic,VIC_CLEAR0,GPT_MASK);
        now=read_reg(csr,GPT_COUNT);
        match=now+ticks;
        write_reg(csr,GPT_MATCH,match);
        drain();
        after=read_reg(csr,GPT_COUNT);
        if((int32_t)(match-after)>(int32_t)LEO_GPT_WRITE_DELAY_TICKS) {
            armed=1;
            write_reg(vic,VIC_ENSET0,GPT_MASK);
            drain();
            ml_set_interrupts_enabled(was_enabled); return;
        }
    }
    panic("Leo GPT compare write repeatedly late");
}

uint64_t Leo_get_timebase(void)
{
    uint64_t ticks;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    ticks=timebase_ready?sample_counter_locked():0;
    ml_set_interrupts_enabled(was_enabled);
    return ticks; /* Hardware GPT ticks; rtclock.c converts to nanoseconds. */
}

uint64_t Leo_timer_value(void)
{
    int32_t remaining=0;
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    if(timebase_ready && armed)
        remaining=(int32_t)(read_reg(csr,GPT_MATCH)-read_reg(csr,GPT_COUNT));
    ml_set_interrupts_enabled(was_enabled);
    return remaining>0?(uint32_t)remaining:0;
}

void Leo_handle_interrupt(void *context)
{
    uint32_t vector, budget;
    if(!controller_ready || !timebase_ready)
        panic("Leo IRQ before controller/timebase initialization");
    for(budget=0;budget<64U;budget++) {
        (void)read_reg(vic,VIC_VECTOR); /* Required latch prime, proven D0/D4. */
        vector=read_reg(vic,VIC_PENDING_VECTOR);
        last_vector=vector;
        if(vector>=64U) {
            if(budget==0) spurious_irq_count++;
            return;
        }
        if(vector!=LEO_GPT_IRQ)
            panic("Leo unexpected enabled IRQ %u",vector);
        write_reg(vic,VIC_ENCLEAR0,GPT_MASK);
        write_reg(vic,VIC_CLEAR0,GPT_MASK); /* ACK edge before original handler. */
        drain();
        armed=0;
        (void)sample_counter_locked();
        last_irq_counter=last_count;
        irq_count++; /* ONLY actual pending hardware IRQ7 reaches this store. */
        rtclock_intr((arm_saved_state_t *)context);
#if !CONFIG_TICKLESS
        Leo_timer_enabled(TRUE);
#endif
        ios7leo_timer_observe(last_irq_counter,irq_count);
    }
    panic("Leo VIC IRQ drain budget exceeded");
}

void Leo_timer_snapshot(struct LeoTimerSnapshot *out)
{
    boolean_t was_enabled=ml_set_interrupts_enabled(FALSE);
    out->version=1;
    out->valid_flags=(controller_ready?1U:0)|(timebase_ready?2U:0)|
        (armed?4U:0)|(counter_observed?8U:0);
    out->count=controller_ready?read_reg(csr,GPT_COUNT):0;
    out->match=controller_ready?read_reg(csr,GPT_MATCH):0;
    out->enable=controller_ready?read_reg(csr,GPT_ENABLE):0;
    out->irq_status=controller_ready?read_reg(vic,VIC_STATUS0):0;
    out->irq_count=irq_count; out->last_irq_counter=last_irq_counter;
    out->last_vector=last_vector; out->spurious_irq_count=spurious_irq_count;
    out->frequency_hz=LEO_GPT_HZ; out->minimum_match_ticks=LEO_GPT_MIN_MATCH_TICKS;
    out->extended_ticks=timebase_ready?sample_counter_locked():0;
    ml_set_interrupts_enabled(was_enabled);
}
#endif
