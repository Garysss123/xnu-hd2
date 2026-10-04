#ifndef _ARM_IOS7_COMPAT_H_
#define _ARM_IOS7_COMPAT_H_

/* Selected software ABI adapters only. This never changes board identity. */
#define IOS7_COMPAT_PLATFORM (BOARD_CONFIG_ARMPBA8 || BOARD_CONFIG_QSD8250_LEO)

#if BOARD_CONFIG_ARMPBA8
#include <pexpert/pexpert.h>
#endif

static inline int ios7_compat_vmabi_enabled(void)
{
#if BOARD_CONFIG_QSD8250_LEO
    return 1;
#elif BOARD_CONFIG_ARMPBA8
    int enabled = 0;
    return PE_parse_boot_argn("ios7lab_vmabi", &enabled, sizeof(enabled)) && enabled == 1;
#else
    return 0;
#endif
}
#endif
