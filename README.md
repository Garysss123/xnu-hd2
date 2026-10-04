# XNU for HTC HD2

XNU kernel and board-support source for the HTC HD2 (HTC Leo, Qualcomm QSD8250).

The HD2 implementation includes CPU/interrupt/timer setup, RAM mapping, SDCC storage, MDP display scanout, and Type2A touch with IOKit display/HID services. The source is based on [HTC-Leo-Revival-Project/xnu](https://github.com/HTC-Leo-Revival-Project/xnu) (`d5c85d9b54bc27c338e69ff75dce2386dcfd723a`) and retains upstream license notices.

`xnu/` contains the kernel tree; HD2 drivers are under `xnu/pexpert/arm/` and `xnu/iokit/Drivers/KernelBuiltIn/ARM/AppleARMPlatform/`. `nokextd/` and `network-port/` contain source-side compatibility headers. The target is `QSD8250_LEO`, ARMv7 Thumb. Building requires an external Darwin/Mach-O ARM toolchain and generated XNU headers; those are not included. The diagnostic nonce header contains a neutral example configuration.

This repository contains source only. Apple userland, root filesystems, caches, firmware, boot images, SDKs, toolchain binaries, runtime logs, and card identities are excluded.

Existing per-file licenses apply. See `xnu/APPLE_LICENSE`, `xnu/LICENSE`, and `LICENSE-NOTICE.md`. No additional blanket license is assigned to files without a license notice.
