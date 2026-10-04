#ifndef IOS7_LEO_FUNCTIONAL_CONTEXT_H
#define IOS7_LEO_FUNCTIONAL_CONTEXT_H

#include <stdint.h>

#define IOS7_LEO_FUNCTIONAL_MAGIC 0x46584e55U
#define IOS7_LEO_FUNCTIONAL_VERSION 1U
#define IOS7_LEO_FUNCTIONAL_PREFIX_BYTES 0x10000U
#define IOS7_LEO_FUNCTIONAL_CONTEXT_OFFSET 0xc000U
#define IOS7_LEO_FUNCTIONAL_CONTEXT_BYTES 1132U
#define IOS7_LEO_FUNCTIONAL_ATAG_LOW 0x11800000U
#define IOS7_LEO_FUNCTIONAL_ATAG_HIGH 0x30000000U
#define IOS7_LEO_FUNCTIONAL_ATAG_LIMIT 4096U
#define IOS7_LEO_FUNCTIONAL_MACHINE 2524U
#define IOS7_LEO_FUNCTIONAL_MAX_BANKS 8U

typedef struct LeoFunctionalRange {
    uint32_t base;
    uint32_t bytes;
} LeoFunctionalRange;

typedef struct LeoFunctionalTags {
    LeoFunctionalRange bank[IOS7_LEO_FUNCTIONAL_MAX_BANKS];
    uint32_t banks;
    LeoFunctionalRange initrd;
    uint32_t atags_base;
    uint32_t atags_bytes;
    char command[256];
} LeoFunctionalTags;

typedef struct LeoFunctionalSegment {
    uint32_t vm;
    uint32_t bytes;
    uint32_t file;
    uint32_t files;
    uint32_t prot;
} LeoFunctionalSegment;

typedef struct LeoFunctionalPayload {
    uint32_t mach_off, mach_bytes, dt_off, dt_bytes, entry;
    uint32_t low_vm, high_vm, segments, dt_handoff;
    LeoFunctionalSegment seg[32];
} LeoFunctionalPayload;

typedef struct LeoFunctionalPlan {
    uint32_t phys, mem, kernel_bytes, dt, boot_args, top, reserved_end, entry;
} LeoFunctionalPlan;

/* Exact mirror of functional.h::struct functional_context.  The context is
 * retained in the validated loaded prefix at runtime_base+0xc000; no pointer
 * in this mirror is dereferenced. */
typedef struct LeoFunctionalContext {
    uint32_t r0,r1,r2,cpsr,sctlr,midr,runtime_base,entry_lr;
    uint32_t total_bytes,prefix_bytes,container_offset,container_bytes;
    uint32_t effective_cpsr,effective_sctlr,phase,error;
    uint32_t failed_va,failed_par,identity_pages,display_profile;
    uint32_t valid_mask;
    LeoFunctionalTags tags;
    LeoFunctionalPayload payload;
    LeoFunctionalPlan plan;
} LeoFunctionalContext;

typedef char leo_functional_context_size_check[
    (sizeof(LeoFunctionalContext)==IOS7_LEO_FUNCTIONAL_CONTEXT_BYTES)?1:-1];
typedef char leo_functional_context_tags_offset_check[
    (__builtin_offsetof(LeoFunctionalContext,tags)==84U)?1:-1];
typedef char leo_functional_context_plan_offset_check[
    (__builtin_offsetof(LeoFunctionalContext,plan)==1100U)?1:-1];

#endif
