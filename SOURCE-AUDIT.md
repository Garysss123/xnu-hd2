# Static source completeness audit

This source-only review covers XNU ARM source-list paths for `QSD8250_LEO`, local include resolution, checked non-ARM source lists, and the SHA-256 manifest. It does not compile code, preprocess every target option, or establish boot or hardware acceptance.

Run from the repository root with Python 3:

```sh
python3 tools/check_source_completeness.py
```

The checker verifies manifest hashes, checks source paths in the architecture-specific `files.arm` lists, and follows local includes from present common and ARM list entries. It also reports absent rows in common source lists and potential unresolved includes. Optional configuration guards are not evaluated, so a warning is a candidate for review, not proof that the active target needs that file.

## Findings

- All 125 source paths in the checked ARM-specific lists are present. This includes the seven IOKit storage implementation files referenced by `xnu/iokit/conf/files.arm`; all 13 matching IOKit storage headers are also included.
- Two additional target-local interface headers are included at `xnu/pexpert/pexpert/arm/leo_entry.h` and `leo_handoff.h`. They are authored source headers from the Leo kernel source tree, not generated outputs or replacement stubs. Their contents match the corresponding build-export copies. `pe_qsd8250_leo.c` includes both; `arm_init.c` includes `leo_entry.h`. See `xnu/pexpert/arm/pe_qsd8250_leo.c` and `xnu/osfmk/arm/arm_init.c`.
- The checked common `files` lists have 43 absent source rows: 39 generated-output candidates and four optional SL/TUN/PPP rows covering three distinct paths. The separately inspected `QSD8250_LEO` build configuration records SL, TUN, and PPP disabled, so those rows are not active for that configuration. That build configuration is not included in this source export, and the static checker does not infer options from the target.
- The generated-output candidates are build products and should be produced from their retained inputs using XNU's existing rules, not filled with hand-written stubs. For example, `xnu/bsd/kern/makesyscalls.sh` defines generated syscall and audit outputs, `xnu/osfmk/mach/Makefile` defines MIG header generation, and `xnu/SETUP/config/mkioconf.c` writes `ioconf.c`. The source export does not include the Darwin/Mach-O ARM SDK/toolchain or all generated build headers; the original target-config build environment is required to regenerate and compile them.
- Some include warnings remain because the checker follows potential ARM and common-source includes without applying optional-source or preprocessor conditions. Generated MIG headers and headers referenced only by other board configurations can therefore appear unresolved. Review the specific source and condition before treating any such warning as an active target dependency.
- The latest static pass traverses 1,862 source/header files and reports 330 unresolved local include references. This is a potential-closure count, not 330 confirmed target build failures; many references are generated MIG outputs or depend on options and preprocessing conditions.
- Other-architecture diagnostics are out of scope for the HD2 target: `files.arm64` refers to absent `osfmk/arm/machroutines.c`, and i386/x86_64 each refer to absent optional `osfmk/kern/etap_map.c`. They were not present in the inspected source base.

The source tree preserves imported XNU and IOKit license notices. The two Leo interface headers have no individual license block; `LICENSE-NOTICE.md` describes the repository's notice policy and asserts no additional blanket license. Source-list completeness and a valid manifest do not prove compiler, linker, QEMU, or physical-device success.
