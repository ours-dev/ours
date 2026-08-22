#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Convert the loadable segments of an ELF file into a flat binary image."""

import argparse
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("objcopy")
    parser.add_argument("elf")
    parser.add_argument("output")
    parser.add_argument("depfile")
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [args.objcopy, "-O", "binary", args.elf, args.output], check=True
    )

    depfile = pathlib.Path(args.depfile)
    depfile.parent.mkdir(parents=True, exist_ok=True)
    depfile.write_text(f"{output}: {args.elf}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
