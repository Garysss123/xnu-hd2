# Static source completeness audit

This source-only review covers the ARM-specific XNU source lists relevant to the `QSD8250_LEO` target, the other checked architecture lists, local include closure, and the published SHA-256 manifest. It does not compile code or infer every `optional` setting in the target configuration.

Run from the repository root with Python 3:

```sh
python3 tools/check_source_completeness.py
```

The checker verifies that every entry in `SOURCE-MANIFEST.sha256` matches the checkout and that each source path in the ARM-specific `files.arm` lists exists. It follows resolvable quoted and project-local angle includes from the ARM source-list entries. Its output also reports generic `files` rows that are absent, other architecture gaps, and local includes that cannot be resolved from this source-only export.

## Current findings

- All 125 source paths in the checked ARM-specific lists are present, including the seven storage sources referenced by `xnu/iokit/conf/files.arm`. The export also now includes all 13 headers from the matching IOKit storage header directory.
- The generic `files` lists contain 43 source rows without a file in this checkout: 39 `./...` generated-output candidates and four optional SL/TUN/PPP networking rows. This static check cannot determine which optional settings were enabled in a particular `QSD8250_LEO` configuration.
- The recursive ARM include scan visits 653 source/header files and follows potential ARM list entries without applying `optional` configuration filters. In particular, `xnu/pexpert/arm/pe_qsd8250_leo.c` includes `pexpert/arm/leo_entry.h` and `pexpert/arm/leo_handoff.h`, and `xnu/osfmk/arm/arm_init.c` also includes `pexpert/arm/leo_entry.h`; neither header is present in this export or in the inspected source/build-export trees. Their implementation and provenance are unresolved, so the public tree remains incomplete for a reproducible `QSD8250_LEO` build until those dependencies are supplied and reviewed.
- The include scan also reports Mach Interface Generator output headers absent from the source tree. Those are generated build products and are not substituted with stubs here. Other unresolved entries can belong to optional board sources; check each warning against the selected target configuration.
- The other architecture lists are diagnostic only: `files.arm64` references absent `osfmk/arm/machroutines.c`, and the i386/x86_64 lists each reference absent optional `osfmk/kern/etap_map.c`. These were not present in the inspected source base and do not establish a gap in the HD2 ARM target.

An audit warning is not proof that an include is required by the active configuration. A clean manifest and complete `files.arm` path check do not establish compiler, linker, boot, or hardware acceptance. The Darwin/Mach-O ARM toolchain, generated XNU headers, and target-specific configuration are outside this source-only repository.
