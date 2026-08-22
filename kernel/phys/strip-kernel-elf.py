#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Produce a section-header-free ELF image for a kernel package."""

import argparse
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("objcopy")
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("depfile")
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [args.objcopy, "--strip-sections", args.input, args.output], check=True
    )

    depfile = pathlib.Path(args.depfile)
    depfile.parent.mkdir(parents=True, exist_ok=True)
    depfile.write_text(f"{output}: {args.input}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
