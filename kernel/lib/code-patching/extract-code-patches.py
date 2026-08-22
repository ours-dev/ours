#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Extract the .code-patches section from a kernel ELF.

A kernel that links no patchable code carries no such section.  That is an
empty patch set, not an error: the packaged payload is zero bytes and
physboot applies nothing.  Any other objcopy failure still fails the build.
"""

import argparse
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("objcopy", help="path to llvm-objcopy")
    parser.add_argument("elf", help="kernel ELF input")
    parser.add_argument("output", help="raw code-patches output")
    parser.add_argument("depfile", help="Ninja depfile output")
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            args.objcopy,
            f"--dump-section=.code-patches={output}",
            args.elf,
            "/dev/null",
        ],
        check=False,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        if "section '.code-patches' not found" not in result.stderr:
            parser.error(result.stderr.strip())
        output.write_bytes(b"")
    elif not output.is_file():
        output.write_bytes(b"")

    depfile = pathlib.Path(args.depfile)
    depfile.parent.mkdir(parents=True, exist_ok=True)
    depfile.write_text(f"{output}: {args.elf}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
