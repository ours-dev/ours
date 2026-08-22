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
"""

import argparse
import os
import re
import shutil
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

    os.execv(
        linker, [linker, *normalized_arguments(linker_arguments, parsed.flavor)]
    )
    return 127


if __name__ == "__main__":
    raise SystemExit(main())
