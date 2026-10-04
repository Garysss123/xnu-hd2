#ifndef IOS7LAB_FAULT_WITNESS_SHARED_H
#define IOS7LAB_FAULT_WITNESS_SHARED_H

#include <stdint.h>

#define IOS7LAB_FAULT_WITNESS_IRQ_JOBS         10U
#define IOS7LAB_FAULT_WITNESS_IRQ_OTHER        10U
#define IOS7LAB_FAULT_WITNESS_IRQ_RING         16U
#define IOS7LAB_FAULT_WITNESS_MAX_CPUS          1U
#define IOS7LAB_FAULT_PAYLOAD_MAX             436U

typedef struct ios7lab_irq_job_snapshot {
    uint32_t role;
    uint32_t pid;
    uint32_t count;
    uint32_t last_pc;
    uint32_t last_sp;
    uint32_t last_lr;
    uint32_t last_cpsr;
    uint64_t tid;
} ios7lab_irq_job_snapshot;

enum {
    IOS7LAB_IRQ_ROLE_SPRINGBOARD = 0,
    IOS7LAB_IRQ_ROLE_BACKBOARDD,
    IOS7LAB_IRQ_ROLE_MSTREAMD,
    IOS7LAB_IRQ_ROLE_INSTALLD,
    IOS7LAB_IRQ_ROLE_LSD,
    IOS7LAB_IRQ_ROLE_SYSLOGD,
    IOS7LAB_IRQ_ROLE_ASSISTIVETOUCHD,
    IOS7LAB_IRQ_ROLE_NOTIFYD,
    IOS7LAB_IRQ_ROLE_MOBILEGESTALT,
    IOS7LAB_IRQ_ROLE_DISTNOTED,
    IOS7LAB_IRQ_ROLE_OTHER,
};

typedef struct ios7lab_irq_context_snapshot {
    uint32_t sequence;
    uint32_t cpu;
    uint32_t pid;
    uint32_t pc;
    uint32_t sp;
    uint32_t lr;
    uint32_t cpsr;
    uint64_t tid;
} ios7lab_irq_context_snapshot;

typedef struct ios7lab_irq_summary {
    uint32_t user_irqs;
    uint32_t kernel_irqs;
    uint32_t unknown_irqs;
    uint32_t saturated;
    uint32_t ring_head;
    uint32_t ring_loss;
    ios7lab_irq_job_snapshot jobs[IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U];
    ios7lab_irq_context_snapshot ring[IOS7LAB_FAULT_WITNESS_IRQ_RING];
} ios7lab_irq_summary;

/* Reader-only APIs. They do not expose ARM saved-state or producer layouts. */
uint32_t ios7lab_fault_witness_take(uint8_t *payload,
                                    uint32_t capacity,
                                    uint32_t *lost,
                                    uint32_t *unknown);
uint32_t ios7lab_irq_snapshot(ios7lab_irq_summary *out);
const char *ios7lab_irq_role_name(uint32_t role);

#endif
