#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Invoke a GNU ELF linker from an Ours Clang-oriented GN toolchain.

GN configs are shared with Clang-driver toolchains, so their linker flags can
also contain compiler-only options and lld-only niceties.  A physical boot
image links directly with a target GNU ld; normalize that small boundary here.
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
}
_LLD_ONLY_FLAGS = {"--color-diagnostics", "--icf=all"}


def normalized_arguments(arguments: list[str]) -> list[str]:
    result: list[str] = []
    for argument in arguments:
        if argument in _DRIVER_FLAGS or re.fullmatch(r"-O[0-3sgz]", argument):
            continue

        if argument.startswith("-Wl,"):
            result.extend(argument[len("-Wl,") :].split(","))
            continue

        result.append(argument)

    return [argument for argument in result if argument not in _LLD_ONLY_FLAGS]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linker", required=True)
    parsed, linker_arguments = parser.parse_known_args()

    linker = shutil.which(parsed.linker)
    if linker is None:
        print(
            f"error: required ELF linker '{parsed.linker}' was not found in PATH",
            file=sys.stderr,
        )
        return 1

    os.execv(linker, [linker, *normalized_arguments(linker_arguments)])
    return 127


if __name__ == "__main__":
    raise SystemExit(main())
