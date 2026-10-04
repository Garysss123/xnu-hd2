#ifndef IOS7_NO_KEXTD_035_H
#define IOS7_NO_KEXTD_035_H
/* Scoped to the five actual LEO conditional translation units. This explicit
 * source prefix follows generated numeric feature preincludes; command-line
 * -D alone would not override an inherited no_kextd.h defining zero. */
#if !defined(BOARD_CONFIG_QSD8250_LEO)
#error IOS7NoKextd035 requires the LEO compilation target
#endif
#ifdef NO_KEXTD
#undef NO_KEXTD
#endif
#define NO_KEXTD 1
#endif
