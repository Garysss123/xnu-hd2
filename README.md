# XNU for HTC HD2

XNU kernel and board-support source for the HTC HD2 (HTC Leo, Qualcomm QSD8250).

The source contains HD2-specific CPU/interrupt/timer setup, RAM mapping, SDCC storage, MDP display scanout, and Type2A touch paths, with IOKit display/HID services. It is based on [HTC-Leo-Revival-Project/xnu](https://github.com/HTC-Leo-Revival-Project/xnu) (`d5c85d9b54bc27c338e69ff75dce2386dcfd723a`) and retains upstream license notices.

`xnu/` contains the kernel tree; HD2 sources are under `xnu/pexpert/arm/` and `xnu/iokit/Drivers/KernelBuiltIn/ARM/AppleARMPlatform/`. `nokextd/` and `network-port/` contain source-side compatibility headers. The target is `QSD8250_LEO`, ARMv7 Thumb. This is not a standalone build package: building requires an external Darwin/Mach-O ARM toolchain, generated XNU headers, and the matching target configuration. The static audit checks source-list paths and available local include closure; it does not establish that the tree compiles, boots, or runs on hardware. See `SOURCE-AUDIT.md` for generated and configuration-dependent gaps. The diagnostic nonce header contains a neutral example configuration.

This repository contains source only. Apple userland, root filesystems, caches, firmware, boot images, SDKs, toolchain binaries, runtime logs, and card identities are excluded.

Existing per-file licenses apply. See `xnu/APPLE_LICENSE`, `xnu/LICENSE`, and `LICENSE-NOTICE.md`. No additional blanket license is assigned to files without a license notice.
