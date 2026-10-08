# cLK raw-image handoff

## Output

- `mach_kernel`: a private copy of the existing plain 32-bit ARM Mach-O XNU kernel. It is not an Image3, `zImage`, or rebuilt kernel.
- DeviceTree: not staged. No standalone DeviceTree was available from a trusted extraction/build path, so there is no DT output to hand off yet.

The public branch remains source-only. The MAGLDR `zImage`/`initrd` trampoline is external packaging, not code in this XNU repository; providing the raw kernel bypasses that package for a loader that accepts Mach-O. No XNU source edit can remove the external trampoline itself.

## Loader contract and blocker

Static inspection confirms the private kernel is a 32-bit ARM Mach-O executable. Its XNU entry consumes `boot_args` in `r0` ([locore](xnu/osfmk/arm/locore.s#L84-L112)). The current Leo early path also requires valid video and machine fields plus a 128-byte `ios7lab-leo-handoff` property under `/chosen` ([checks](xnu/pexpert/arm/pe_qsd8250_leo.c#L512-L531), [handoff record](xnu/pexpert/arm/pe_qsd8250_leo.c#L778-L808)). The cLK loader must provide the matching contract and a compatible DeviceTree. This kernel is HD2/Leo-specific; the raw Mach-O format does not make it a Nexus One port.

GenericBooter at [`e38e050393a840903b587310a8ca584d21a8d1bb`](https://github.com/HTC-Leo-Revival-Project/GenericBooter/tree/e38e050393a840903b587310a8ca584d21a8d1bb) expects Image3-wrapped inputs in its documented path ([upstream README](https://github.com/HTC-Leo-Revival-Project/GenericBooter/blob/e38e050393a840903b587310a8ca584d21a8d1bb/README.md#L37-L46)); this raw handoff is for cLK's direct Mach-O path.

## Build and verification

No reproducible clean build or raw DeviceTree output is available from this repository. The staged kernel is copied from the existing 070 output and only statically identified; it was not rebuilt, booted, or tested. No QEMU or device operation was performed.
