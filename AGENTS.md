# Branch scope

- Keep this cLK handoff work in the isolated `codex/hd2-genericbooter` checkout. Do not rewrite, reset, or replace `codex/hd2-source` or its worktree.
- Keep this repository source-only. Do not add Apple binaries, DeviceTrees, ramdisks, firmware, boot images, private logs, device identifiers, or other private project payloads.
- Do not copy or vendor third-party bootloader code into this tree.
- Record only directly verified artifact/build/boot evidence. Static format checks are not boot evidence.
- Preserve `SOURCE-MANIFEST.sha256`: when tracked files change, regenerate its sorted SHA-256 rows for every tracked file except the manifest itself, then verify all rows and paths.
- Keep private binaries and local artifact paths outside the public repository.
