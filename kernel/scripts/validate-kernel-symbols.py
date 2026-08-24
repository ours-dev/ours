#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Reject function-local static guard variables in the linked kernel."""

import argparse
import pathlib
import subprocess


_BANNED_PREFIX = "_ZGVZ"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nm", help="path to llvm-nm")
    parser.add_argument("kernel_rsp", help="file containing the kernel ELF path")
    parser.add_argument("output", help="stamp file to create after validation")
    parser.add_argument("depfile", help="Ninja depfile to create")
    args = parser.parse_args()

    rsp_path = pathlib.Path(args.kernel_rsp)
    kernel_path = pathlib.Path(rsp_path.read_text(encoding="utf-8").strip())
    output_path = pathlib.Path(args.output)
    depfile_path = pathlib.Path(args.depfile)

    depfile_path.parent.mkdir(parents=True, exist_ok=True)
    depfile_path.write_text(
        f"{output_path}: {rsp_path} {kernel_path}\n", encoding="utf-8"
    )

    result = subprocess.run(
        [args.nm, "-j", kernel_path],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    banned = [
        symbol for symbol in result.stdout.splitlines() if symbol.startswith(_BANNED_PREFIX)
    ]
    if banned:
        parser.error(
            f"{kernel_path} contains non-trivial function-local statics:\n"
            + "\n".join(banned)
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.touch()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
