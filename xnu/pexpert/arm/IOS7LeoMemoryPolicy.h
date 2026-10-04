/* iOS-lab owned bounded RAM admission policy for the ordinary HTC Leo.
 *
 * This is a board-profile admission, not a physical RAM probe.  The raw
 * ATAG_MEM table remains evidence and is never rewritten.  Only the exact
 * one-bank legacy table observed on hardware may use the published
 * CONFIG_USING_BRAVOS_DSP-safe EBI1 end. */
#ifndef IOS7_LEO_MEMORY_POLICY_H
#define IOS7_LEO_MEMORY_POLICY_H

#define IOS7_LEO_MEMORY_PROFILE_NONE 0U
#define IOS7_LEO_MEMORY_PROFILE_LEGACY64 1U
#define IOS7_LEO_MEMORY_PROFILE_RAW_BASE 0x11800000U
#define IOS7_LEO_MEMORY_PROFILE_RAW_BYTES 0x04000000U
#define IOS7_LEO_MEMORY_PROFILE_SAFE_END 0x2e7c0000U
#define IOS7_LEO_MEMORY_PROFILE_ALIGN 0x00100000U

typedef struct IOS7LeoMemoryPolicyRange {
    unsigned int base;
    unsigned int bytes;
} IOS7LeoMemoryPolicyRange;

static inline int ios7leo_memory_policy_range_end(
    IOS7LeoMemoryPolicyRange range, unsigned int *end)
{
    if (!range.bytes || range.base > 0xffffffffU - range.bytes)
        return 0;
    *end = range.base + range.bytes;
    return 1;
}

static inline int ios7leo_memory_policy_contains(
    IOS7LeoMemoryPolicyRange outer, IOS7LeoMemoryPolicyRange inner)
{
    unsigned int outer_end, inner_end;
    return ios7leo_memory_policy_range_end(outer, &outer_end) &&
           ios7leo_memory_policy_range_end(inner, &inner_end) &&
           inner.base >= outer.base && inner_end <= outer_end;
}

static inline int ios7leo_memory_policy_overlaps(
    IOS7LeoMemoryPolicyRange a, IOS7LeoMemoryPolicyRange b)
{
    unsigned int a_end, b_end;
    if (!ios7leo_memory_policy_range_end(a, &a_end) ||
        !ios7leo_memory_policy_range_end(b, &b_end))
        return 1;
    return a.base < b_end && b.base < a_end;
}

static inline int ios7leo_memory_policy_align_up(unsigned int value,
                                                 unsigned int *aligned)
{
    if (value > 0xffffffffU - (IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U))
        return 0;
    *aligned = (value + IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U) &
               ~(IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U);
    return 1;
}

static inline unsigned int ios7leo_memory_policy_window(
    unsigned int bank_count, unsigned int bank0_base,
    unsigned int bank0_bytes, IOS7LeoMemoryPolicyRange *window)
{
    if (!window || bank_count != 1U ||
        bank0_base != IOS7_LEO_MEMORY_PROFILE_RAW_BASE ||
        bank0_bytes != IOS7_LEO_MEMORY_PROFILE_RAW_BYTES)
        return IOS7_LEO_MEMORY_PROFILE_NONE;
    window->base = IOS7_LEO_MEMORY_PROFILE_RAW_BASE;
    window->bytes = IOS7_LEO_MEMORY_PROFILE_SAFE_END -
                    IOS7_LEO_MEMORY_PROFILE_RAW_BASE;
    return IOS7_LEO_MEMORY_PROFILE_LEGACY64;
}

/* Independently validate the exact board-admitted suffix.  The order of the
 * two exclusions matches the frontend planner.  Loader is checked separately
 * even though it must be inside INITRD, so no future caller can accidentally
 * admit a plan over still-running prefix/context/stack bytes. */
static inline unsigned int ios7leo_memory_policy_expected_plan(
    unsigned int bank_count, unsigned int bank0_base,
    unsigned int bank0_bytes, IOS7LeoMemoryPolicyRange initrd,
    IOS7LeoMemoryPolicyRange atags, IOS7LeoMemoryPolicyRange loader,
    IOS7LeoMemoryPolicyRange plan)
{
    IOS7LeoMemoryPolicyRange raw, window, suffix, exclude[2];
    unsigned int base, end, exclude_end, size, plan_end, i;
    unsigned int profile = ios7leo_memory_policy_window(
        bank_count, bank0_base, bank0_bytes, &window);
    if (profile == IOS7_LEO_MEMORY_PROFILE_NONE)
        return profile;
    raw.base = bank0_base;
    raw.bytes = bank0_bytes;
    if (!ios7leo_memory_policy_contains(raw, initrd) ||
        !ios7leo_memory_policy_contains(raw, atags) ||
        !ios7leo_memory_policy_contains(raw, loader) ||
        !ios7leo_memory_policy_contains(initrd, loader) ||
        ios7leo_memory_policy_overlaps(initrd, atags) ||
        !ios7leo_memory_policy_align_up(window.base, &base))
        return IOS7_LEO_MEMORY_PROFILE_NONE;
    exclude[0] = initrd;
    exclude[1] = atags;
    if (!ios7leo_memory_policy_range_end(window, &end))
        return IOS7_LEO_MEMORY_PROFILE_NONE;
    for (i = 0; i < 2U; ++i) {
        if (base >= end)
            return IOS7_LEO_MEMORY_PROFILE_NONE;
        suffix.base = base;
        suffix.bytes = end - base;
        if (ios7leo_memory_policy_overlaps(suffix, exclude[i])) {
            if (!ios7leo_memory_policy_range_end(exclude[i], &exclude_end) ||
                exclude_end <= base ||
                !ios7leo_memory_policy_align_up(exclude_end, &base))
                return IOS7_LEO_MEMORY_PROFILE_NONE;
        }
    }
    if (base >= end)
        return IOS7_LEO_MEMORY_PROFILE_NONE;
    size = (end - base) & ~(IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U);
    if (!size || (plan.base & (IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U)) ||
        (plan.bytes & (IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U)) ||
        plan.base != base || plan.bytes != size ||
        !ios7leo_memory_policy_range_end(plan, &plan_end) ||
        plan_end != (end & ~(IOS7_LEO_MEMORY_PROFILE_ALIGN - 1U)) ||
        ios7leo_memory_policy_overlaps(plan, initrd) ||
        ios7leo_memory_policy_overlaps(plan, atags) ||
        ios7leo_memory_policy_overlaps(plan, loader))
        return IOS7_LEO_MEMORY_PROFILE_NONE;
    return profile;
}

typedef char ios7leo_memory_policy_uint32[
    (sizeof(unsigned int) == 4U) ? 1 : -1];

#endif
