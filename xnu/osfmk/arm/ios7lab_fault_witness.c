#include <mach/mach_types.h>
#include <mach/exception_types.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/misc_protos.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <vm/vm_map.h>
#include <libsa/string.h>

#include "ios7lab_fault_witness.h"

#define IOS7LAB_COPY_NOT_ATTEMPTED 0xffffffffU
#define IOS7LAB_USER_TOP            0x40000000U
#define IOS7LAB_VALID_DYLD_MASK     0x0000000fU
#define IOS7LAB_DYLD_HEADER_OK      0x00000001U
#define IOS7LAB_DYLD_PARSE_OK       0x00000002U
#define IOS7LAB_DYLD_TAIL_OK        0x00000004U
#define IOS7LAB_DYLD_EMPTY_OK       0x00000008U
#define IOS7LAB_FAULT_PAYLOAD_MAX   436U

typedef struct ios7lab_irq_state {
    volatile uint32_t ring_head;
    volatile uint32_t ring_loss;
    volatile uint32_t user_irqs;
    volatile uint32_t kernel_irqs;
    volatile uint32_t unknown_irqs;
    volatile uint32_t saturated;
    ios7lab_irq_job_snapshot jobs[IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U];
    ios7lab_irq_context_snapshot ring[IOS7LAB_FAULT_WITNESS_IRQ_RING];
} ios7lab_irq_state;

typedef struct ios7lab_fault_ring {
    volatile uint32_t claimed;
    volatile uint32_t ready;
    volatile uint32_t loss;
    volatile uint32_t unknown;
    volatile uint32_t emit_stage;
    ios7lab_fault_record record;
} ios7lab_fault_ring;

static ios7lab_fault_ring ios7lab_fault_store;
static ios7lab_irq_state ios7lab_irq_states[IOS7LAB_FAULT_WITNESS_MAX_CPUS];
typedef char ios7lab_bss_budget[(sizeof(ios7lab_fault_ring) + sizeof(ios7lab_irq_states) <= 8192U) ? 1 : -1];

static void
ios7lab_sat_inc(volatile uint32_t *value, volatile uint32_t *saturated)
{
    uint32_t old;
    do {
        old = *value;
        if (old == 0xffffffffU) {
            if (saturated)
                *saturated = 1U;
            return;
        }
    } while (!OSCompareAndSwap(old, old + 1U, value));
}

static const uint8_t ios7lab_dyld_parse[IOS7LAB_FAULT_WITNESS_DYLD_PARSE_BYTES] = {
    0xf0,0xb5,0x03,0xaf,0x2d,0xe9,0x00,0x0d,
    0x8c,0xb0,0x06,0x46,0x34,0x46,0x14,0xf8
};
static const uint8_t ios7lab_dyld_tail[IOS7LAB_FAULT_WITNESS_DYLD_TAIL_BYTES] = {
    0x0c,0xb0,0xbd,0xe8,0x00,0x0d,0xbd,0xe8,
    0xf0,0x40,0x48,0x47
};
static const uint8_t ios7lab_dyld_empty[IOS7LAB_FAULT_WITNESS_DYLD_EMPTY_BYTES] = {
    0x70,0x47,0xc0,0x46
};

static const char *const ios7lab_irq_role_names[IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U] = {
    "SpringBoard", "backboardd", "mstreamd", "installd", "lsd",
    "syslogd", "assistivetouchd", "notifyd", "MobileGestalt", "distnoted",
    "other"
};

const char *
ios7lab_irq_role_name(uint32_t role)
{
    return role <= IOS7LAB_FAULT_WITNESS_IRQ_OTHER ? ios7lab_irq_role_names[role] : "other";
}

static boolean_t
ios7lab_user_range(uint32_t address, uint32_t bytes)
{
    if (address < 0x1000U || bytes > IOS7LAB_USER_TOP)
        return FALSE;
    return address <= IOS7LAB_USER_TOP - bytes;
}

static boolean_t
ios7lab_proc_is_springboard(thread_t thread)
{
    const char *name;
    if (!thread || !thread->task || !thread->task->bsd_info)
        return FALSE;
    name = proc_name_address(thread->task->bsd_info);
    return name && proc_pid(thread->task->bsd_info) > 0 &&
           !strncmp(name, "SpringBoard", 12U);
}

boolean_t
ios7lab_fault_witness_is_springboard(thread_t thread,
                                     const abort_information_context_t *context)
{
    if (!context || (context->cpsr & 0x1fU) != 0x10U)
        return FALSE;
    return ios7lab_proc_is_springboard(thread);
}

static uint32_t
ios7lab_job_pid(thread_t thread)
{
    if (!thread || !thread->task || !thread->task->bsd_info)
        return 0;
    return (uint32_t)proc_pid(thread->task->bsd_info);
}

static uint32_t
ios7lab_irq_role(thread_t thread)
{
    const char *name;
    if (!thread || !thread->task || !thread->task->bsd_info)
        return IOS7LAB_FAULT_WITNESS_IRQ_OTHER;
    name = proc_name_address(thread->task->bsd_info);
    if (!name) return IOS7LAB_IRQ_ROLE_OTHER;
    if (!strncmp(name, "SpringBoard", 12U)) return IOS7LAB_IRQ_ROLE_SPRINGBOARD;
    if (!strncmp(name, "backboardd", 10U)) return IOS7LAB_IRQ_ROLE_BACKBOARDD;
    if (!strncmp(name, "mstreamd", 8U)) return IOS7LAB_IRQ_ROLE_MSTREAMD;
    if (!strncmp(name, "installd", 8U)) return IOS7LAB_IRQ_ROLE_INSTALLD;
    if (!strncmp(name, "lsd", 3U)) return IOS7LAB_IRQ_ROLE_LSD;
    if (!strncmp(name, "syslogd", 7U)) return IOS7LAB_IRQ_ROLE_SYSLOGD;
    if (!strncmp(name, "assistivetouchd", 15U)) return IOS7LAB_IRQ_ROLE_ASSISTIVETOUCHD;
    if (!strncmp(name, "notifyd", 7U)) return IOS7LAB_IRQ_ROLE_NOTIFYD;
    if (!strncmp(name, "MobileGestalt", 13U)) return IOS7LAB_IRQ_ROLE_MOBILEGESTALT;
    if (!strncmp(name, "distnoted", 9U)) return IOS7LAB_IRQ_ROLE_DISTNOTED;
    return IOS7LAB_IRQ_ROLE_OTHER;
}

static ios7lab_irq_state *
ios7lab_current_irq_state(void)
{
    unsigned cpu = (unsigned)cpu_number();
    if (cpu >= IOS7LAB_FAULT_WITNESS_MAX_CPUS)
        cpu = 0;
    return &ios7lab_irq_states[cpu];
}

static void
ios7lab_irq_record_job(ios7lab_irq_state *state,
                       thread_t thread,
                       const arm_saved_state_t *saved)
{
    uint32_t pid = ios7lab_job_pid(thread);
    uint64_t tid = thread ? thread->thread_id : 0;
    uint32_t role = ios7lab_irq_role(thread);
    ios7lab_irq_job_snapshot *job = &state->jobs[role];
    job->role = role;
    ios7lab_sat_inc(&job->count, &state->saturated);
    job->pid = pid;
    job->tid = tid;
    job->last_pc = saved->pc;
    job->last_sp = saved->sp;
    job->last_lr = saved->lr;
    job->last_cpsr = saved->cpsr;
}

void
ios7lab_irq_observe(void *context)
{
    arm_saved_state_t *saved = (arm_saved_state_t *)context;
    thread_t thread = current_thread();
    ios7lab_irq_state *state;
    uint32_t seq;
    ios7lab_irq_context_snapshot *slot;

    state = ios7lab_current_irq_state();
    if (!saved || !thread) {
        ios7lab_sat_inc(&state->unknown_irqs, &state->saturated);
        return;
    }
    if ((saved->cpsr & 0x1fU) != 0x10U) {
        ios7lab_sat_inc(&state->kernel_irqs, &state->saturated);
        return;
    }
    ios7lab_sat_inc(&state->user_irqs, &state->saturated);
    ios7lab_irq_record_job(state, thread, saved);
    if (!ios7lab_proc_is_springboard(thread))
        return;

    seq = (uint32_t)OSIncrementAtomic((volatile SInt32 *)&state->ring_head) + 1U;
    if (seq == 0U) {
        ios7lab_sat_inc(&state->ring_loss, &state->saturated);
        return;
    }
    if (seq > IOS7LAB_FAULT_WITNESS_IRQ_RING)
        ios7lab_sat_inc(&state->ring_loss, &state->saturated);
    slot = &state->ring[(seq - 1U) & (IOS7LAB_FAULT_WITNESS_IRQ_RING - 1U)];
    slot->cpu = (uint32_t)cpu_number();
    slot->pid = ios7lab_job_pid(thread);
    slot->tid = thread->thread_id;
    slot->pc = saved->pc;
    slot->sp = saved->sp;
    slot->lr = saved->lr;
    slot->cpsr = saved->cpsr;
    OSMemoryBarrier();
    slot->sequence = seq;
}

uint32_t
ios7lab_irq_snapshot(ios7lab_irq_summary *out)
{
    ios7lab_irq_state *state;
    boolean_t previous;
    unsigned i;
    if (!out)
        return 0U;
    state = ios7lab_current_irq_state();
    previous = ml_set_interrupts_enabled(FALSE);
    out->user_irqs = state->user_irqs;
    out->kernel_irqs = state->kernel_irqs;
    out->unknown_irqs = state->unknown_irqs;
    out->saturated = state->saturated;
    out->ring_head = state->ring_head;
    out->ring_loss = state->ring_loss;
    for (i = 0; i < IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U; ++i)
        out->jobs[i] = state->jobs[i];
    for (i = 0; i < IOS7LAB_FAULT_WITNESS_IRQ_RING; ++i)
        out->ring[i] = state->ring[i];
    ml_set_interrupts_enabled(previous);
    return 1U;
}

static void
ios7lab_copy_code(uint32_t *error,
                  uint8_t *destination,
                  uint32_t address,
                  const uint8_t *expected,
                  uint32_t bytes,
                  uint32_t *valid_mask,
                  uint32_t valid_bit)
{
    int result;
    if (!ios7lab_user_range(address, bytes)) {
        *error = KERN_INVALID_ADDRESS;
        return;
    }
    bzero(destination, bytes);
    result = copyin((user_addr_t)address, destination, bytes);
    *error = (uint32_t)result;
    if (result != 0) {
        bzero(destination, bytes);
    } else if (!bcmp(destination, expected, bytes))
        *valid_mask |= valid_bit;
}

static boolean_t
ios7lab_dyld_runtime_address(uint32_t base, uint32_t canonical,
                             uint32_t *runtime)
{
    if (base > IOS7LAB_USER_TOP - canonical)
        return FALSE;
    *runtime = base + canonical;
    return TRUE;
}

static void
ios7lab_copy_dyld(ios7lab_fault_record *record, task_t task)
{
    ios7lab_dyld32_prefix prefix;
    uint32_t base, magic = 0;
    int result;

    record->dyld_prefix_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    record->dyld_header_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    record->dyld_parse_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    record->dyld_tail_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    record->dyld_empty_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    bzero(record->dyld_prefix, sizeof(record->dyld_prefix));
    bzero(record->dyld_parse, sizeof(record->dyld_parse));
    bzero(record->dyld_tail, sizeof(record->dyld_tail));
    bzero(record->dyld_empty, sizeof(record->dyld_empty));
    if (!task || !task->all_image_info_addr ||
        (uint64_t)task->all_image_info_addr > 0xffffffffULL ||
        task->all_image_info_size < IOS7LAB_FAULT_WITNESS_DYLD_PREFIX_BYTES ||
        task->all_image_info_size > 0x400U ||
        !ios7lab_user_range((uint32_t)task->all_image_info_addr,
                            IOS7LAB_FAULT_WITNESS_DYLD_PREFIX_BYTES))
        return;
    record->all_image_info_addr = (uint32_t)task->all_image_info_addr;
    record->all_image_info_size = (uint32_t)task->all_image_info_size;
    bzero(&prefix, sizeof(prefix));
    bzero(&prefix, sizeof(prefix));
    result = copyin((user_addr_t)task->all_image_info_addr,
                    &prefix, sizeof(prefix));
    record->dyld_prefix_error = (uint32_t)result;
    if (result) {
        bzero(record->dyld_prefix, sizeof(record->dyld_prefix));
        return;
    }
    bcopy(&prefix, record->dyld_prefix, sizeof(prefix));
    base = prefix.dyldImageLoadAddress;
    record->dyld_base = base;
    if ((base & 3U) != 0U || !ios7lab_user_range(base, sizeof(magic))) {
        record->dyld_header_error = KERN_INVALID_ADDRESS;
        return;
    }
    result = copyin((user_addr_t)base, &magic, sizeof(magic));
    record->dyld_header_error = (uint32_t)result;
    if (result)
        return;
    if (magic != 0xFEEDFACEU && magic != 0xCEFAEDFEU)
        return;
    record->dyld_valid_mask |= IOS7LAB_DYLD_HEADER_OK;
    {
        uint32_t address;
        if (ios7lab_dyld_runtime_address(base, IOS7LAB_DYLD_PARSE_OFFSET, &address))
            ios7lab_copy_code(&record->dyld_parse_error, record->dyld_parse,
                              address, ios7lab_dyld_parse, sizeof(ios7lab_dyld_parse),
                              &record->dyld_valid_mask, IOS7LAB_DYLD_PARSE_OK);
        else
            record->dyld_parse_error = KERN_INVALID_ADDRESS;
        if (ios7lab_dyld_runtime_address(base, IOS7LAB_DYLD_TAIL_OFFSET, &address))
            ios7lab_copy_code(&record->dyld_tail_error, record->dyld_tail,
                              address, ios7lab_dyld_tail, sizeof(ios7lab_dyld_tail),
                              &record->dyld_valid_mask, IOS7LAB_DYLD_TAIL_OK);
        else
            record->dyld_tail_error = KERN_INVALID_ADDRESS;
        if (ios7lab_dyld_runtime_address(base, IOS7LAB_DYLD_EMPTY_OFFSET, &address))
            ios7lab_copy_code(&record->dyld_empty_error, record->dyld_empty,
                              address, ios7lab_dyld_empty, sizeof(ios7lab_dyld_empty),
                              &record->dyld_valid_mask, IOS7LAB_DYLD_EMPTY_OK);
        else
            record->dyld_empty_error = KERN_INVALID_ADDRESS;
    }
}

boolean_t
ios7lab_fault_witness_capture(thread_t thread,
                              const abort_information_context_t *context,
                              uint32_t ifsr,
                              uint32_t ifar,
                              kern_return_t fault_result,
                              uint32_t old_exception_code,
                              uint32_t old_exception_subcode)
{
    uint32_t stack_start;
    ios7lab_irq_summary irq_summary;
    ios7lab_fault_record *record;
    int stack_error;

    if (!ios7lab_fault_witness_is_springboard(thread, context) ||
        !OSCompareAndSwap(0U, 1U, &ios7lab_fault_store.claimed))
        return FALSE;
    record = &ios7lab_fault_store.record;
    bzero(record, sizeof(*record));
    record->magic = IOS7LAB_FAULT_WITNESS_MAGIC;
    record->version = IOS7LAB_FAULT_WITNESS_VERSION;
    record->sequence = 1U;
    record->cpu = (uint32_t)cpu_number();
    record->pid = ios7lab_job_pid(thread);
    record->tid = thread ? thread->thread_id : 0;
    record->reason = 3U;
    record->old_exception_code = old_exception_code;
    record->old_exception_subcode = old_exception_subcode;
    record->corrected_exception_code = (uint32_t)fault_result;
    record->corrected_exception_subcode = context->pc;
    record->corrected_valid = 1U;
    record->fault_result = (uint32_t)fault_result;
    record->ifsr = ifsr;
    record->ifar = ifar;
    record->context = *context;
    record->stack_copy_error = IOS7LAB_COPY_NOT_ATTEMPTED;
    record->dyld_valid_mask = 0U;
    bzero(&irq_summary, sizeof(irq_summary));
    if (ios7lab_irq_snapshot(&irq_summary)) {
        record->irq_head = irq_summary.ring_head;
        record->irq_loss = irq_summary.ring_loss;
        record->irq_user = irq_summary.user_irqs;
        record->irq_kernel = irq_summary.kernel_irqs;
        record->irq_unknown = irq_summary.unknown_irqs;
        record->irq_saturated = irq_summary.saturated;
        bcopy(irq_summary.jobs, record->jobs, sizeof(record->jobs));
        bcopy(irq_summary.ring, record->irq, sizeof(record->irq));
    }
    if ((context->sp & 3U) == 0U &&
        context->sp >= 0x1000U + IOS7LAB_FAULT_WITNESS_STACK_BEFORE &&
        context->sp <= IOS7LAB_USER_TOP - IOS7LAB_FAULT_WITNESS_STACK_AFTER) {
        stack_start = context->sp - IOS7LAB_FAULT_WITNESS_STACK_BEFORE;
        record->stack_start = stack_start;
        record->stack_bytes = IOS7LAB_FAULT_WITNESS_STACK_BYTES;
        bzero(record->stack, sizeof(record->stack));
        stack_error = copyin((user_addr_t)stack_start, record->stack,
                             IOS7LAB_FAULT_WITNESS_STACK_BYTES);
        record->stack_copy_error = (uint32_t)stack_error;
        record->stack_valid = stack_error == 0 ? 1U : 0U;
        if (stack_error)
            bzero(record->stack, sizeof(record->stack));
    } else {
        record->stack_start = 0U;
        record->stack_bytes = IOS7LAB_FAULT_WITNESS_STACK_BYTES;
        record->stack_copy_error = KERN_INVALID_ADDRESS;
        record->stack_valid = 0U;
        bzero(record->stack, sizeof(record->stack));
    }
    ios7lab_copy_dyld(record, thread ? thread->task : TASK_NULL);
    ios7lab_fault_store.emit_stage = 0U;
    OSMemoryBarrier();
    ios7lab_fault_store.ready = 1U;
    return TRUE;
}

static uint32_t
ios7lab_hex(uint8_t *out, uint32_t capacity, const uint8_t *bytes, uint32_t count)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t i;
    if (capacity == 0U || count > (capacity - 1U) / 2U)
        return 0U;
    for (i = 0; i < count; ++i) {
        out[2U * i] = digits[bytes[i] >> 4];
        out[2U * i + 1U] = digits[bytes[i] & 0xfU];
    }
    out[2U * count] = 0;
    return 2U * count;
}

static uint32_t
ios7lab_emit_fault(ios7lab_fault_record *record,
                   uint32_t stage,
                   uint8_t *payload,
                   uint32_t capacity)
{
    uint32_t part, start, count, used, i;
    char hex[IOS7LAB_FAULT_WITNESS_STACK_BYTES * 2U + 1U];
    if (stage == 0U) {
        return (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT V=%u PID=%u TID=%llu CPU=%u REASON=%u OLD=%08x/%08x NEW=%08x/%08x RESULT=%08x IFSR=%08x IFAR=%08x PC=%08x LR=%08x SP=%08x CPSR=%08x STACK=%u/%u/%08x DYLD=%08x/%08x MASK=%x IRQ=%u/%u/%u/%u\n",
            record->version, record->pid, (unsigned long long)record->tid,
            record->cpu, record->reason, record->old_exception_code,
            record->old_exception_subcode, record->corrected_exception_code,
            record->corrected_exception_subcode, record->fault_result,
            record->ifsr, record->ifar, record->context.pc, record->context.lr,
            record->context.sp, record->context.cpsr, record->stack_valid,
            record->stack_bytes, record->stack_copy_error, record->dyld_base,
            record->all_image_info_addr, record->dyld_valid_mask,
            record->irq_user, record->irq_kernel, record->irq_unknown,
            record->irq_saturated);
    }
    if (stage == 1U) {
        used = (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT REGS ");
        for (i = 0; i < 13U; ++i) {
            if (used >= capacity)
                return used;
            used += (uint32_t)snprintf((char *)payload + used, capacity - used,
                                       "R%u=%08x ", i, record->context.r[i]);
        }
        if (used + 1U < capacity) {
            payload[used++] = '\n'; payload[used] = 0;
        }
        return used;
    }
    if (stage >= 2U && stage <= 5U) {
        part = stage - 2U;
        start = part * 48U;
        count = IOS7LAB_FAULT_WITNESS_STACK_BYTES - start;
        if (count > 48U)
            count = 48U;
        ios7lab_hex((uint8_t *)hex, sizeof(hex), record->stack + start, count);
        return (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT STACK PART=%u START=%08x BYTES=%u VALID=%u DATA=%s\n",
            part, record->stack_start + start, count, record->stack_valid, hex);
    }
    if (stage == 6U) {
        ios7lab_hex((uint8_t *)hex, sizeof(hex), record->dyld_parse,
                    sizeof(record->dyld_parse));
        used = (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT DYLD PREFIX_ERR=%08x HEADER_ERR=%08x PARSE_ERR=%08x TAIL_ERR=%08x EMPTY_ERR=%08x BASE=%08x INFO=%08x/%08x MASK=%x PARSE=%s ",
            record->dyld_prefix_error, record->dyld_header_error,
            record->dyld_parse_error, record->dyld_tail_error,
            record->dyld_empty_error, record->dyld_base,
            record->all_image_info_addr, record->all_image_info_size,
            record->dyld_valid_mask, hex);
        if (used >= capacity)
            return used;
        ios7lab_hex(payload + used, capacity - used, record->dyld_tail,
                    sizeof(record->dyld_tail));
        used += (uint32_t)strlen((char *)payload + used);
        if (used + 1U < capacity) {
            payload[used++] = ' ';
            ios7lab_hex(payload + used, capacity - used, record->dyld_empty,
                        sizeof(record->dyld_empty));
            used += (uint32_t)strlen((char *)payload + used);
        }
        if (used + 1U < capacity)
            payload[used++] = '\n';
        if (used < capacity)
            payload[used] = 0;
        return used;
    }
    if (stage >= 7U && stage <= 9U) {
        start = (stage - 7U) * 4U;
        used = (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT JOBS PART=%u ", stage - 7U);
        for (i = 0; i < 4U && start + i < IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U; ++i) {
            ios7lab_irq_job_snapshot *job = &record->jobs[start + i];
            if (used >= capacity)
                return used;
            used += (uint32_t)snprintf((char *)payload + used, capacity - used,
                "[%u,%u,%llu,%u,%08x,%08x,%08x,%08x] ", job->role,
                job->pid, (unsigned long long)job->tid, job->count,
                job->last_pc, job->last_sp, job->last_lr, job->last_cpsr);
        }
        if (used + 1U < capacity) {
            payload[used++] = '\n'; payload[used] = 0;
        }
        return used;
    }
    if (stage >= 10U && stage <= 13U) {
        start = (stage - 10U) * 4U;
        used = (uint32_t)snprintf((char *)payload, capacity,
            "IOS7LAB FIRSTFAULT IRQ PART=%u ", stage - 10U);
        for (i = 0; i < 4U; ++i) {
            ios7lab_irq_context_snapshot *sample = &record->irq[(start + i) &
                (IOS7LAB_FAULT_WITNESS_IRQ_RING - 1U)];
            if (used >= capacity)
                return used;
            used += (uint32_t)snprintf((char *)payload + used, capacity - used,
                "[%u,%u,%u,%llu,%08x,%08x,%08x,%08x] ", sample->sequence,
                sample->cpu, sample->pid, (unsigned long long)sample->tid,
                sample->pc, sample->sp, sample->lr, sample->cpsr);
        }
        if (used + 1U < capacity) {
            payload[used++] = '\n'; payload[used] = 0;
        }
        return used;
    }
    return 0U;
}

uint32_t
ios7lab_fault_witness_take(uint8_t *payload, uint32_t capacity,
                           uint32_t *lost, uint32_t *unknown)
{
    uint32_t stage;
    ios7lab_fault_record *record;
    if (!payload || capacity < 32U || !lost || !unknown)
        return 0U;
    *lost = ios7lab_fault_store.loss;
    *unknown = ios7lab_fault_store.unknown;
    if (!ios7lab_fault_store.ready)
        return 0U;
    record = &ios7lab_fault_store.record;
    if (record->magic != IOS7LAB_FAULT_WITNESS_MAGIC ||
        record->sequence != 1U) {
        ios7lab_fault_store.unknown = 1U;
        ios7lab_fault_store.ready = 0U;
        ios7lab_fault_store.emit_stage = 14U;
        return 0U;
    }
    stage = ios7lab_fault_store.emit_stage;
    if (stage > 13U) {
        ios7lab_fault_store.ready = 0U;
        ios7lab_fault_store.emit_stage = 0U;
        return 0U;
    }
    {
        uint32_t bytes = ios7lab_emit_fault(record, stage, payload, capacity);
        if (bytes == 0U || bytes >= capacity) {
            ios7lab_fault_store.unknown = 1U;
            ios7lab_fault_store.ready = 0U;
            ios7lab_fault_store.emit_stage = 14U;
            return 0U;
        }
        ios7lab_fault_store.emit_stage = stage + 1U;
        if (stage == 13U)
            ios7lab_fault_store.emit_stage = 14U;
        return bytes;
    }
}
