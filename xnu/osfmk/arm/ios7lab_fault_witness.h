#ifndef IOS7LAB_FAULT_WITNESS_H
#define IOS7LAB_FAULT_WITNESS_H

#include <stdint.h>
#include <mach/boolean.h>
#include <mach/mach_types.h>
#include <arm/misc_protos.h>
#include "ios7lab_fault_witness_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOS7LAB_FAULT_WITNESS_MAGIC             0x53333146U /* S31F */
#define IOS7LAB_FAULT_WITNESS_VERSION           1U
#define IOS7LAB_FAULT_WITNESS_STACK_BYTES       192U
#define IOS7LAB_FAULT_WITNESS_STACK_BEFORE      128U
#define IOS7LAB_FAULT_WITNESS_STACK_AFTER       64U
#define IOS7LAB_FAULT_WITNESS_DYLD_PREFIX_BYTES 24U
#define IOS7LAB_FAULT_WITNESS_DYLD_PARSE_BYTES  16U
#define IOS7LAB_FAULT_WITNESS_DYLD_TAIL_BYTES   12U
#define IOS7LAB_FAULT_WITNESS_DYLD_EMPTY_BYTES  4U
#define IOS7LAB_FAULT_WITNESS_FAULT_RING        1U
#define IOS7LAB_FAULT_WITNESS_MAX_STAGES       14U

/* The canonical original dyld image base used by the bounded 030 evidence. */
#define IOS7LAB_DYLD_CANONICAL_BASE             0x2BE00000U
#define IOS7LAB_DYLD_PARSE_OFFSET               0x0000BC1CU
#define IOS7LAB_DYLD_TAIL_OFFSET                0x0000BF38U
#define IOS7LAB_DYLD_EMPTY_OFFSET               0x00013FECU

/* dyld_all_image_infos prefix through dyldImageLoadAddress.  The current
 * task reports a larger 0xA4 structure; only this fixed 24-byte prefix is
 * consumed so an older private KDP layout cannot alter this ABI. */
typedef struct ios7lab_dyld32_prefix {
    uint32_t version;
    uint32_t infoArrayCount;
    uint32_t infoArray;
    uint32_t notification;
    uint8_t processDetachedFromSharedRegion;
    uint8_t libSystemInitialized;
    uint8_t reserved[2];
    uint32_t dyldImageLoadAddress;
} ios7lab_dyld32_prefix;

typedef struct ios7lab_fault_record {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t cpu;
    uint32_t pid;
    uint64_t tid;
    uint32_t reason;
    uint32_t old_exception_code;
    uint32_t old_exception_subcode;
    uint32_t corrected_exception_code;
    uint32_t corrected_exception_subcode;
    uint32_t corrected_valid;
    uint32_t fault_result;
    uint32_t ifsr;
    uint32_t ifar;
    uint32_t stack_start;
    uint32_t stack_bytes;
    uint32_t stack_copy_error;
    uint32_t stack_valid;
    uint32_t all_image_info_addr;
    uint32_t all_image_info_size;
    uint32_t dyld_prefix_error;
    uint32_t dyld_header_error;
    uint32_t dyld_parse_error;
    uint32_t dyld_tail_error;
    uint32_t dyld_empty_error;
    uint32_t dyld_valid_mask;
    uint32_t dyld_base;
    uint32_t irq_loss;
    uint32_t irq_unknown;
    uint32_t irq_head;
    uint32_t irq_user;
    uint32_t irq_kernel;
    uint32_t irq_saturated;
    uint8_t dyld_prefix[IOS7LAB_FAULT_WITNESS_DYLD_PREFIX_BYTES];
    uint8_t dyld_parse[IOS7LAB_FAULT_WITNESS_DYLD_PARSE_BYTES];
    uint8_t dyld_tail[IOS7LAB_FAULT_WITNESS_DYLD_TAIL_BYTES];
    uint8_t dyld_empty[IOS7LAB_FAULT_WITNESS_DYLD_EMPTY_BYTES];
    abort_information_context_t context;
    uint8_t stack[IOS7LAB_FAULT_WITNESS_STACK_BYTES];
    ios7lab_irq_job_snapshot jobs[IOS7LAB_FAULT_WITNESS_IRQ_JOBS + 1U];
    ios7lab_irq_context_snapshot irq[IOS7LAB_FAULT_WITNESS_IRQ_RING];
} ios7lab_fault_record;

typedef char ios7lab_fault_record_word_aligned[(sizeof(ios7lab_fault_record) % 4U) == 0U ? 1 : -1];

boolean_t ios7lab_fault_witness_is_springboard(thread_t thread,
                                                const abort_information_context_t *context);

/* Call only after the existing interrupt-enable point and before doexception. */
boolean_t ios7lab_fault_witness_capture(thread_t thread,
                                         const abort_information_context_t *context,
                                         uint32_t ifsr,
                                         uint32_t ifar,
                                         kern_return_t fault_result,
                                         uint32_t old_exception_code,
                                         uint32_t old_exception_subcode);

/* IRQ producer: saved-state/identity tuple only; no user reads or formatting. */
void ios7lab_irq_observe(void *context);

#ifdef __cplusplus
}
#endif

#endif
