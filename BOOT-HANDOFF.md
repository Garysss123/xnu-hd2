# Plain Mach-O and DeviceTree handoff

## Private inputs

- `mach_kernel` is a private copy of the existing plain 32-bit ARM Mach-O XNU kernel. It is not an Image3, `zImage`, or rebuilt kernel.
- `DeviceTree.leo.bin` is a standalone Apple-flat DeviceTree input recorded in the existing 070 package. The private handoff folder contains the tree beside the kernel; this source repository contains neither binary.
- The tree has one 128-byte `/chosen/ios7lab-leo-handoff` property. Its bytes are all zero in the staged input: it is a placeholder, not a completed runtime record.

## Loader contract

XNU entry consumes `boot_args` in `r0` ([locore](xnu/osfmk/arm/locore.s#L84-L112)). The current Leo early path requires valid video and machine fields plus the 128-byte property under `/chosen` ([checks](xnu/pexpert/arm/pe_qsd8250_leo.c#L512-L531), [record schema](xnu/pexpert/arm/pe_qsd8250_leo.c#L778-L808)). A loader or bridge must populate that record from live CPU state, framebuffer/DMA state, RAM layout, kernel placement, initrd bounds, and ATAGS before entering XNU. The existing local bridge constructor does this at runtime; the static DeviceTree alone does not.

The MAGLDR `zImage`/`initrd` trampoline is external packaging, not code in this XNU repository. A raw Mach-O only bypasses that package when the selected loader accepts the file format and supplies the required boot contract. Compatibility of this raw pair with cLK has not been verified. The pinned [GenericBooter README](https://github.com/HTC-Leo-Revival-Project/GenericBooter/blob/e38e050393a840903b587310a8ca584d21a8d1bb/README.md#L37-L46) documents Image3-wrapped kernel and DeviceTree inputs, so the raw pair does not match that documented input format as-is.

## Evidence limits

The kernel was copied from an existing 070 output and was not rebuilt. The DeviceTree is the existing 070 packaging input, not a capture of live device state. Neither artifact has a claimed boot result; no QEMU or device operation is reported here. This is an HD2/Leo kernel, not a Nexus One port.
