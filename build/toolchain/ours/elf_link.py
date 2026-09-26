#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Invoke an ELF linker directly from an Ours Clang-oriented GN toolchain.

GN link commands are written for a compiler driver, so they carry
compiler-only options and wrap linker options in `-Wl,`.  Freestanding images
link the linker directly instead -- the Clang driver delegates an ELF-target
link to the host `gcc`, which cannot build an image for another architecture.
Normalize that small boundary here.

`--flavor lld` keeps lld-only options; `--flavor gnu` drops them so the same
GN configs can also drive a target GNU ld.

It also refuses a link whose inputs break an invariant the linker script cannot
state for itself -- see check_init_rodata().
"""

import argparse
import os
import re
import shutil
import struct
import sys


_DRIVER_FLAGS = {
    "-fcolor-diagnostics",
    "-fdata-sections",
    "-ffreestanding",
    "-ffunction-sections",
    "-fno-exceptions",
    "-fno-omit-frame-pointer",
    "-fno-rtti",
    "-mcmodel=kernel",
    "-mno-red-zone",
    "-momit-leaf-frame-pointer",
    "-nolibc",
    "-nostartfiles",
    "-nostdlib",
    "-nostdlib++",
    "-static-libgcc",
    "-static-libstdc++",
}
_LLD_ONLY_FLAGS = {"--color-diagnostics", "--icf=all"}

# Compiler-only switches that take a value; `ld` would misparse the leading
# `-m` as its emulation-mode selector.
_DRIVER_FLAG_PREFIXES = ("-march=", "-mtune=", "-mcmodel=")


def normalized_arguments(arguments: list[str], flavor: str) -> list[str]:
    result: list[str] = []
    for argument in arguments:
        if argument.startswith("-Wl,"):
            result.extend(argument[len("-Wl,") :].split(","))
            continue

        if (
            argument in _DRIVER_FLAGS
            or argument.startswith(_DRIVER_FLAG_PREFIXES)
            # Diagnostics are the compiler's business.  -Wl, is the one
            # exception and has already been unwrapped above.
            or argument.startswith("-W")
            or re.fullmatch(r"-O[0-3sgz]", argument)
        ):
            continue

        result.append(argument)

    if flavor == "lld":
        return result
    return [argument for argument in result if argument not in _LLD_ONLY_FLAGS]




_SHF_WRITE = 0x1
_SHT_REL = 9
_SHT_RELA = 4
_ELF_MAGIC = b"\x7fELF"
_ARCHIVE_MAGIC = b"!<arch>\n"

# The section whose name promises read-only boot-time data; see INIT_RODATA in
# kernel/include/ours/init.hpp.
_INIT_RODATA = ".init.rodata"


def _sections(image: bytes) -> list[tuple[str, int, int, int]]:
    """(name, sh_type, sh_flags, sh_info) for every section of one ELF object.

    Hand-rolled rather than shelled out to llvm-readelf: this runs on every
    input of every link, and readelf's output is a text format that would have
    to be re-parsed whenever it changes.  Only the section header table is
    touched, so the cost is a few struct unpacks per object.
    """
    if len(image) < 64 or image[:4] != _ELF_MAGIC:
        return []

    is_64 = image[4] == 2
    endian = "<" if image[5] == 1 else ">"
    # e_shoff, then e_shentsize/e_shnum/e_shstrndx, at the offsets the two ELF
    # classes put them at. The `<`/`>` prefix also turns off struct's alignment
    # padding, which is what makes the section-header formats below match the
    # on-disk layout exactly.
    if is_64:
        (shoff,) = struct.unpack_from(endian + "Q", image, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(endian + "HHH", image, 0x3A)
        header = endian + "IIQQQQIIQQ"
    else:
        (shoff,) = struct.unpack_from(endian + "I", image, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(endian + "HHH", image, 0x2E)
        header = endian + "IIIIIIIIII"

    if shoff == 0 or shnum == 0:
        # A real object can carry its section count in section 0 (SHN_XINDEX),
        # which this does not need to handle: no compiler in this build emits
        # 65280 sections in one translation unit.
        return []

    def entry(index: int) -> tuple[int, ...]:
        return struct.unpack_from(header, image, shoff + index * shentsize)

    if shstrndx >= shnum:
        return []
    strtab_offset, strtab_size = (entry(shstrndx)[4], entry(shstrndx)[5])
    strtab = image[strtab_offset : strtab_offset + strtab_size]

    def name_at(offset: int) -> str:
        end = strtab.find(b"\0", offset)
        return strtab[offset:end].decode("utf-8", "replace") if end >= 0 else ""

    result = []
    for index in range(shnum):
        sh_name, sh_type, sh_flags, _addr, _off, _size, _link, sh_info = entry(index)[:8]
        result.append((name_at(sh_name), sh_type, sh_flags, sh_info))
    return result


def _objects(path: str) -> list[tuple[str, bytes]]:
    """(display name, bytes) for `path`: itself, or each member if it is an archive."""
    try:
        with open(path, "rb") as stream:
            blob = stream.read()
    except OSError:
        return []

    if blob[: len(_ELF_MAGIC)] == _ELF_MAGIC:
        return [(path, blob)]
    if blob[: len(_ARCHIVE_MAGIC)] != _ARCHIVE_MAGIC:
        return []

    members: list[tuple[str, bytes]] = []
    long_names = b""
    cursor = len(_ARCHIVE_MAGIC)
    while cursor + 60 <= len(blob):
        raw_name = blob[cursor : cursor + 16].rstrip()
        try:
            size = int(blob[cursor + 48 : cursor + 58].strip())
        except ValueError:
            break
        body = blob[cursor + 60 : cursor + 60 + size]
        if raw_name == b"//":
            long_names = body
        elif raw_name not in (b"/", b"/SYM64/"):
            name = raw_name.decode("utf-8", "replace").rstrip("/")
            if raw_name.startswith(b"/") and long_names:
                offset = int(raw_name[1:])
                end = long_names.find(b"/", offset)
                name = long_names[offset:end].decode("utf-8", "replace")
            members.append((f"{path}({name})", body))
        cursor += 60 + size + (size & 1)
    return members


def check_init_rodata(inputs: list[str]) -> list[str]:
    """Inputs that put writable, unrelocated data in `.init.rodata`.

    The section's name promises read-only and nothing enforces it. Two different
    things make it writable, and only one is a mistake:

    - A missing `const` on the object. INIT_RODATA is a link-section attribute
      and confers no constness of its own -- a macro cannot, because a
      prepended `const` binds to the pointee and would be silently wrong for
      pointers -- so the keyword is written at the declaration and can be
      forgotten.
    - Data that holds pointers. In this DYN image those need dynamic
      relocations, so the toolchain marks the section writable however `const`
      the object is. That is not an error: physboot applies the relocations
      through the identity map of the image's physical pages, never through the
      kernel mapping, so the pages can still be mapped read-only.

    Hence the rule: writable *and* nothing relocates it. It has to be checked
    per object file, because after the link `.init.rodata` is one merged section
    and its provenance is gone.

    What that covers, exactly:

    - A translation unit whose `.init.rodata` holds only mutable,
      relocation-free data is caught here.
    - One that mixes a mutable object with a read-only one never reaches the
      linker: clang refuses it with "causes a section type conflict", because
      the two want different flags on the same explicitly named section.
    - One whose `.init.rodata` holds only mutable *pointer-bearing* data cannot
      be caught by any link-time check: with and without `const` the object file
      is byte for byte identical, flags and relocations included. There the
      compiler is the enforcement, at the write site.

    This lives here rather than in kernel.ld because a linker script cannot say
    it. lld's expression language has no way to read a section's flags, and it
    rejects both of GNU ld's ways of getting at them: `INPUT_SECTION_FLAGS(
    SHF_WRITE)` ("section pattern is expected") and the `(READONLY)`
    output-section attribute ("symbol not found: READONLY").
    """
    violations = []
    for path in inputs:
        for display, blob in _objects(path):
            sections = _sections(blob)
            relocated = {
                sh_info
                for _name, sh_type, _flags, sh_info in sections
                if sh_type in (_SHT_REL, _SHT_RELA)
            }
            for index, (name, _type, flags, _info) in enumerate(sections):
                if name != _INIT_RODATA and not name.startswith(_INIT_RODATA + "."):
                    continue
                if not flags & _SHF_WRITE or index in relocated:
                    continue
                violations.append(f"{display}: {name} is writable")
    return violations


def _link_inputs(arguments: list[str]) -> list[str]:
    """Object and archive paths the linker will read, response files expanded."""
    inputs = []
    for argument in arguments:
        if argument.startswith("@"):
            try:
                with open(argument[1:], encoding="utf-8") as stream:
                    inputs.extend(stream.read().split())
            except OSError:
                continue
        elif argument.endswith((".o", ".a", ".obj")):
            inputs.append(argument)
    return inputs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linker", required=True)
    parser.add_argument("--flavor", choices=["lld", "gnu"], default="gnu")
    parsed, linker_arguments = parser.parse_known_args()

    linker = shutil.which(parsed.linker)
    if linker is None:
        print(
            f"error: required ELF linker '{parsed.linker}' was not found in PATH",
            file=sys.stderr,
        )
        return 1

    arguments = normalized_arguments(linker_arguments, parsed.flavor)

    violations = check_init_rodata(_link_inputs(arguments))
    if violations:
        print(
            "error: writable data with no relocations in .init.rodata -- the\n"
            "       declaration is missing `const` (INIT_RODATA only names the\n"
            "       section; see kernel/include/ours/init.hpp):",
            file=sys.stderr,
        )
        for violation in violations:
            print(f"       {violation}", file=sys.stderr)
        return 1

    os.execv(
        linker, [linker, *normalized_arguments(linker_arguments, parsed.flavor)]
    )
    return 127


if __name__ == "__main__":
    raise SystemExit(main())
