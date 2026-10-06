# Static source completeness audit

This source-only review covers the XNU ARM source-list paths for `QSD8250_LEO`, local include resolution, checked non-ARM source lists, and the SHA-256 manifest. It does not compile code, preprocess every target option, or establish boot or hardware acceptance.

Run from the repository root with Python 3:

```sh
python3 tools/check_source_completeness.py
```

The checker verifies manifest hashes, checks source paths in architecture-specific `files.arm` lists, follows locally resolvable includes from present common and ARM list entries, and reports absent common-list rows and potential unresolved includes. It strips C/C++ comments before scanning, but it does not evaluate target options or preprocessor conditions. A reported include is therefore a candidate reference, not proof of an active missing dependency.

## Findings

- All 125 source paths in the checked ARM-specific lists are present. This includes the seven IOKit storage implementation files listed by `xnu/iokit/conf/files.arm`; all 13 matching IOKit storage headers are also included.
- Two additional target-local interface headers are included at `xnu/pexpert/pexpert/arm/leo_entry.h` and `leo_handoff.h`. They are source-authored headers from the Leo kernel tree, not generated outputs or replacement stubs. Their contents match the corresponding build-export copies. `pe_qsd8250_leo.c` includes both; `arm_init.c` includes `leo_entry.h`.
- The checked common `files` lists contain 43 absent rows: 39 generated-output candidates and four optional SL/TUN/PPP entries covering three distinct paths. The existing `QSD8250_LEO` configuration headers set SL, TUN, and PPP to 0. That makes the rows inactive in the inspected configuration. The source rows are at `xnu/bsd/conf/files:213-229`. An `if_tun.o` artifact is also present in the inspected target output despite TUN being disabled, so the artifact alone cannot establish whether it was stale or linked; this source-only audit does not resolve that discrepancy.
- The 39 `./...` rows are build outputs, not source files to recreate by hand: four BSD outputs (`audit_kevents.c`, `init_sysent.c`, `ioconf.c`, and `syscalls.c`) and 35 OSF/MIG outputs. The inspected target build/export output contains corresponding generated files. The public tree retains their inputs and rules, including `xnu/bsd/kern/makesyscalls.sh`, `xnu/osfmk/mach/Makefile`, and `xnu/SETUP/config/mkioconf.c`. Reproducing them requires the matching target configuration and Darwin/Mach-O ARM toolchain, which are not part of this source export.
- The comment-aware static scan visits 1,862 source/header files and reports 329 unresolved local include edges. Comparison against the existing QSD8250_LEO debug/export headers and the public source tree classifies them as follows:

| Candidate edges | Count | Evidence and interpretation |
| --- | ---: | --- |
| `mach/...` names found in target debug/export header output | 279 | These span 48 names and are generated or exported target headers, largely MIG products. The source retains the `.defs` inputs and MIG generation rules; the generated header tree is not included in the source archive. |
| `libkern/version.h` and configuration-generated headers | 16 | Seven references to `libkern/version.h` and nine references to `compat_43.h`, `ether.h`, `loop.h`, `pty.h`, and `vndevice.h`. The inspected target output has these generated headers; they are not missing source-authored files. |
| Existing public headers not found by the simple include-root resolver | 29 | Twenty-two `kxld.h`, `kxld_types.h`, and `zlib.h` references occur in non-kernel branches; `xnu/makedefs/MakeInc.def:244` supplies `-DKERNEL`. Five `mach/mach_init.h` references resolve to the existing public header under `xnu/libsyscall/mach/mach/`; the checker does not model that include root. The Yarrow non-kernel branch (`xnu/bsd/dev/random/YarrowCoreLib/src/comp.c:39-72`) also accounts for two edges to `WindowsTypesForMac.h` and `yarrowUtils.h`, which are already present under `xnu/bsd/dev/random/YarrowCoreLib/include/`. |
| Conditional or other-target references not found in the public tree | 4 | Two `IOKit/IOCFSerialize.h` / `IOCFUnserialize.h` references are guarded by `KERNEL_CF` in `xnu/osfmk/UserNotification/KUNCUserNotifications.c:47-51`; those headers were not found in the inspected source or target export. One `machine/smp.h` reference is guarded by `SMP` in `xnu/bsd/kern/kern_mib.c:77-79`. One `pe_touchpad.h` reference is guarded by `BOARD_CONFIG_MSM8960_TOUCHPAD` in `xnu/pexpert/arm/pe_apq8060.c:60,79`, an alternate board path. These are not evidence of a missing QSD8250_LEO source dependency. |
| Non-kernel branch reference not found in the public tree | 1 | `compat.h` is included in the `#else /* KERNEL */` branch of `xnu/bsd/vfs/vfs_journal.c:106`; the inspected target make definitions set `KERNEL` at `xnu/makedefs/MakeInc.def:244`. |

The classification accounts for the 329 reported edges. `tools/check_source_completeness.py` now removes comments before scanning, so the commented-out `machine/in_cksum.h` text in `xnu/bsd/netinet/ip_fw2.c` no longer appears as a potential include. These static findings do not replace preprocessing the exact target configuration. In particular, the `SMP` and `KERNEL_CF` defines were not established for every possible out-of-tree build variant.

No additional source-authored public dependency was found to restore after reviewing the target source-list and generated/export-header evidence. The Yarrow headers are already included in the public tree with their existing notices; generated outputs, external CoreFoundation headers, and alternate-board headers were not copied into it.

- Other-architecture diagnostics are out of scope for the HD2 target: `files.arm64` refers to absent `osfmk/arm/machroutines.c`, and i386/x86_64 each refer to absent optional `osfmk/kern/etap_map.c`. They were not present in the inspected source base.

The source tree preserves imported XNU and IOKit license notices. The two Leo interface headers have no individual license block; `LICENSE-NOTICE.md` describes the repository's notice policy and assigns no additional blanket license. A complete source-list check and valid manifest do not prove compiler, linker, QEMU, or physical-device success.
