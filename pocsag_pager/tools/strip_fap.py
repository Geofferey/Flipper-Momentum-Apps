#!/usr/bin/env python3
"""Remove the symbol table and legacy relocations from a built .fap.

A FAP keeps two independent relocation formats: the precomputed
.fast.rel.* sections the current loader uses, and the original
.rel.text/.rel.rodata/.rel.data tables it falls back to when those are
absent. It also ships .symtab/.strtab, which the loader only needs for
the fallback path. lib/flipper_application/elf/elf_file.c accepts a file
that has either .symtab plus .strtab or .fast.rel data, and prefers
.fast.rel whenever it is present, so for a fast-loading FAP all of that
is dead weight on the SD card. Stripping it leaves roughly half the file.

Nothing about the loaded application changes: .text, .rodata, .data and
.bss are untouched, so RAM use is identical. The only loss is symbol
names in a crash backtrace, and the unstripped <app>_d.elf next to the
.fap still has those.

Usage:
    python tools/strip_fap.py <file.fap> [...]        # strip in place
    python tools/strip_fap.py <file.fap> -o out.fap   # write elsewhere
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

# Sections the loader needs for a fast-loading FAP.
REQUIRED = (".text", ".fapmeta", ".fast.rel.text")

STRIP_NAME = "arm-none-eabi-strip"


def find_strip() -> str:
    """Locate arm-none-eabi-strip on PATH, else in the firmware toolchain."""
    found = shutil.which(STRIP_NAME)
    if found:
        return found

    # applications/external/<app>/tools/ -> firmware root is four levels up.
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    machine = "x86_64" if platform.machine().lower() in ("amd64", "x86_64") else "arm64"
    host = {"Windows": "windows", "Darwin": "darwin", "Linux": "linux"}.get(platform.system())
    suffix = ".exe" if host == "windows" else ""

    candidates = []
    if host:
        candidates.append(
            os.path.join(root, "toolchain", f"{machine}-{host}", "bin", STRIP_NAME + suffix)
        )
    toolchain = os.path.join(root, "toolchain")
    if os.path.isdir(toolchain):
        for entry in sorted(os.listdir(toolchain)):
            candidates.append(os.path.join(toolchain, entry, "bin", STRIP_NAME + suffix))

    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate

    raise SystemExit(
        f"{STRIP_NAME} not found on PATH or under {toolchain}; "
        "run this from an fbt environment or pass --strip"
    )


def section_names(path: str) -> list[str]:
    """Read the ELF32 section header table and return the section names."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit(f"{path}: not a little-endian 32-bit ELF file")

    sh_offset, = struct.unpack_from("<I", data, 0x20)
    sh_entsize, sh_num, sh_strndx = struct.unpack_from("<HHH", data, 0x2E)
    if not sh_offset or not sh_num:
        raise SystemExit(f"{path}: no section header table")

    def name_offset(index: int) -> int:
        return struct.unpack_from("<I", data, sh_offset + index * sh_entsize)[0]

    strtab_offset = struct.unpack_from("<I", data, sh_offset + sh_strndx * sh_entsize + 0x10)[0]

    names = []
    for index in range(sh_num):
        start = strtab_offset + name_offset(index)
        end = data.index(b"\0", start)
        names.append(data[start:end].decode("ascii", "replace"))
    return names


def strip_one(strip: str, source: str, destination: str) -> tuple[int, int]:
    before = os.path.getsize(source)

    # Strip into a temporary file so a failure cannot leave a partial .fap.
    handle, staged = tempfile.mkstemp(suffix=".fap", dir=os.path.dirname(destination) or ".")
    os.close(handle)
    try:
        subprocess.run([strip, "--strip-all", "-o", staged, source], check=True)

        missing = [name for name in REQUIRED if name not in section_names(staged)]
        if missing:
            raise SystemExit(f"{source}: stripping removed {', '.join(missing)}; refusing to write")

        os.replace(staged, destination)
    except BaseException:
        if os.path.exists(staged):
            os.unlink(staged)
        raise

    return before, os.path.getsize(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("faps", nargs="+", help="built .fap file(s)")
    parser.add_argument("-o", "--output", help="write here instead of stripping in place")
    parser.add_argument("--strip", help=f"path to {STRIP_NAME}")
    args = parser.parse_args()

    if args.output and len(args.faps) > 1:
        parser.error("--output takes a single input file")

    strip = args.strip or find_strip()

    for source in args.faps:
        if not os.path.isfile(source):
            raise SystemExit(f"{source}: not found")
        before, after = strip_one(strip, source, args.output or source)
        print(
            f"{os.path.basename(source)}: {before} -> {after} bytes "
            f"(-{before - after}, -{100.0 * (before - after) / before:.1f}%)"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
