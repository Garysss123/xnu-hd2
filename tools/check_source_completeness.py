#!/usr/bin/env python3
"""Static source-only checks for the published ARM XNU tree.

This does not preprocess XNU configuration files or replace a real build. It
checks source-list paths, follows resolvable local includes, and validates the
published SHA-256 manifest without needing an Apple toolchain.
"""

from __future__ import annotations

import hashlib
import re
import sys
from collections import deque
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / "xnu"
SUBSYSTEMS = ("bsd", "iokit", "libkern", "libsa", "osfmk", "pexpert", "security")
ARCHES = ("arm", "arm64", "i386", "x86_64")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".s", ".asm"}
SCAN_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".h", ".hpp", ".inc"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
LOCAL_ANGLE_PREFIXES = (
    "IOKit/", "bsd/", "kern/", "libkern/", "libsa/", "mach/", "machine/",
    "osfmk/", "pexpert/",
)
INCLUDE_ROOTS = (
    XNU,
    XNU / "bsd",
    XNU / "iokit",
    XNU / "libkern",
    XNU / "libsa",
    XNU / "osfmk",
    XNU / "osfmk" / "kern",
    XNU / "osfmk" / "ipc",
    XNU / "pexpert",
    XNU / "security",
    XNU / "iokit" / "Drivers" / "KernelBuiltIn" / "ARM" / "AppleARMPlatform",
    ROOT / "network-port" / "headers",
    ROOT / "network-port" / "compat",
    ROOT / "nokextd",
)


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def inside_root(path: Path) -> bool:
    try:
        path.relative_to(ROOT)
        return True
    except ValueError:
        return False


def manifest_check() -> tuple[int, list[str]]:
    manifest_path = ROOT / "SOURCE-MANIFEST.sha256"
    errors: list[str] = []
    expected: dict[str, str] = {}
    try:
        rows = manifest_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return 0, [f"cannot read SOURCE-MANIFEST.sha256: {exc}"]

    for number, row in enumerate(rows, start=1):
        if not row.strip():
            continue
        try:
            digest, name = row.split("  ", 1)
        except ValueError:
            errors.append(f"manifest line {number}: malformed row")
            continue
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or name in expected:
            errors.append(f"manifest line {number}: unsafe or duplicate path {name}")
            continue
        expected[name] = digest

    actual_paths = {
        relative(path)
        for path in ROOT.rglob("*")
        if path.is_file() and ".git" not in path.relative_to(ROOT).parts
        and path != manifest_path and not path.is_symlink()
    }
    for name in sorted(expected.keys() - actual_paths):
        errors.append(f"manifest path missing from checkout: {name}")
    for name in sorted(actual_paths - expected.keys()):
        errors.append(f"unmanifested file: {name}")
    for name, wanted in sorted(expected.items()):
        path = ROOT / Path(*PurePosixPath(name).parts)
        if not path.is_file():
            continue
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        if got != wanted:
            errors.append(f"manifest hash mismatch: {name}")
    return len(expected), errors


def list_rows(path: Path) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        name = fields[0]
        if name.startswith("OPTIONS/") or Path(name).suffix.lower() not in SOURCE_SUFFIXES:
            continue
        rows.append((name, " ".join(fields[1:])))
    return rows


def source_path(name: str) -> Path | None:
    # XNU conf/files entries are rooted at the XNU source directory.
    rel = PurePosixPath(name)
    if rel.is_absolute() or ".." in rel.parts:
        return None
    path = (XNU / Path(*rel.parts)).resolve()
    if not inside_root(path):
        return None
    return path


def resolve_include(name: str, parent: Path) -> Path | None:
    rel = PurePosixPath(name)
    if rel.is_absolute():
        return None
    # Relative includes may legitimately use ../; the resolved candidate still
    # must stay inside the published tree.
    candidates = [parent / Path(*rel.parts)]
    candidates.extend(root / Path(*rel.parts) for root in INCLUDE_ROOTS)
    for candidate in candidates:
        resolved = candidate.resolve()
        if inside_root(resolved) and resolved.is_file():
            return resolved
    return None


def strip_comments(text: str) -> str:
    """Blank C/C++ comments while preserving line numbers and string contents."""
    chars = list(text)
    state = "code"
    quote: str | None = None
    escaped = False
    index = 0
    while index < len(chars):
        pair = chars[index : index + 2]
        char = chars[index]
        if state == "line":
            if char == "\n":
                state = "code"
            else:
                chars[index] = " "
        elif state == "block":
            if pair == ["*", "/"]:
                chars[index] = chars[index + 1] = " "
                index += 1
                state = "code"
            elif char not in "\r\n":
                chars[index] = " "
        elif quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in {"'", '"'}:
            quote = char
        elif pair == ["/", "*"]:
            chars[index] = chars[index + 1] = " "
            index += 1
            state = "block"
        elif pair == ["/", "/"]:
            chars[index] = chars[index + 1] = " "
            index += 1
            state = "line"
        index += 1
    return "".join(chars)


def main() -> int:
    hard_errors: list[str] = []
    warnings: list[str] = []

    manifest_count, manifest_errors = manifest_check()
    hard_errors.extend(manifest_errors)

    arch_sources: dict[str, list[Path]] = {}
    arch_missing: dict[str, list[str]] = {}
    for arch in ARCHES:
        sources: list[Path] = []
        missing: list[str] = []
        for subsystem in SUBSYSTEMS:
            filelist = XNU / subsystem / "conf" / f"files.{arch}"
            if not filelist.is_file():
                continue
            for name, _condition in list_rows(filelist):
                path = source_path(name)
                if path is None:
                    missing.append(f"{subsystem}/conf/files.{arch}: unsafe path {name}")
                elif not path.is_file():
                    missing.append(f"{subsystem}/conf/files.{arch}: {name}")
                else:
                    sources.append(path)
        arch_sources[arch] = sources
        arch_missing[arch] = missing
        if arch == "arm":
            hard_errors.extend(f"missing ARM build-list source: {item}" for item in missing)
        else:
            warnings.extend(f"other-architecture source-list gap: {item}" for item in missing)

    common_missing: list[tuple[str, str, str]] = []
    common_sources: list[Path] = []
    common_total = 0
    for subsystem in SUBSYSTEMS:
        filelist = XNU / subsystem / "conf" / "files"
        if not filelist.is_file():
            continue
        for name, condition in list_rows(filelist):
            common_total += 1
            path = source_path(name)
            if path is None or not path.is_file():
                common_missing.append((subsystem, name, condition))
            else:
                common_sources.append(path)

    generated_common = [row for row in common_missing if row[1].startswith("./")]
    conditional_common = [row for row in common_missing if not row[1].startswith("./")]
    if common_missing:
        warnings.append(
            f"common files lists contain {len(common_missing)} absent rows: "
            f"{len(generated_common)} generated-looking ./ outputs and "
            f"{len(conditional_common)} conditional/other rows; selection is not inferred"
        )

    # Follow resolvable quoted includes from present common and ARM source-list
    # entries. The list parser does not evaluate optional/configuration guards.
    # Local angle-bracket includes are followed too; unresolved non-local angle
    # includes are toolchain/SDK candidates.
    queue = deque(arch_sources["arm"] + common_sources)
    visited: set[Path] = set()
    unresolved: set[tuple[str, int, str]] = set()
    while queue:
        source = queue.popleft().resolve()
        if source in visited or not source.is_file() or not inside_root(source):
            continue
        visited.add(source)
        if source.suffix.lower() not in SCAN_SUFFIXES:
            continue
        try:
            text = source.read_text(encoding="utf-8", errors="replace")
            lines = strip_comments(text).splitlines()
        except OSError:
            continue
        for line_number, line in enumerate(lines, start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include_name = match.group(1).strip()
            resolved = resolve_include(include_name, source.parent)
            if resolved is not None:
                if resolved not in visited:
                    queue.append(resolved)
                continue
            if line.lstrip().startswith('#include "') or include_name.startswith(LOCAL_ANGLE_PREFIXES):
                unresolved.add((relative(source), line_number, include_name))

    print(f"Manifest: {manifest_count} entries; {'OK' if not manifest_errors else 'ERROR'}")
    for arch in ARCHES:
        print(
            f"files.{arch}: {len(arch_sources[arch]) + len(arch_missing[arch])} source rows; "
            f"{len(arch_missing[arch])} missing"
        )
    print(
        f"Common files: {common_total} source rows; {len(common_missing)} absent "
        f"({len(generated_common)} ./ generator candidates, {len(conditional_common)} conditional/other)"
    )
    print(f"Potential ARM+common local include closure: {len(visited)} files visited; {len(unresolved)} unresolved local includes")
    for item in sorted(arch_missing["arm"]):
        print(f"ERROR {item}")
    for item in sorted(warnings):
        print(f"WARN {item}")
    for subsystem, name, condition in sorted(common_missing):
        kind = "generated-output" if name.startswith("./") else "conditional-or-other"
        print(f"WARN common source absent ({kind}): {subsystem}/conf/files: {name} [{condition}]")
    for source, line_number, include_name in sorted(unresolved):
        print(f"WARN unresolved include {source}:{line_number}: {include_name}")
    if hard_errors:
        for item in hard_errors:
            if not item.startswith("missing ARM build-list source:"):
                print(f"ERROR {item}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
