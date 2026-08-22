#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Extract exactly one GNU build ID from an ELF binary."""

import argparse
import pathlib
import re
import subprocess


_BUILD_ID = re.compile(r"^\s*Build ID:\s*([0-9a-fA-F]+)\s*$", re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("readelf", help="path to llvm-readelf")
    parser.add_argument("elf", help="ELF input")
    parser.add_argument("output", help="build-id text output")
    parser.add_argument("depfile", help="Ninja depfile output")
    args = parser.parse_args()

    result = subprocess.run(
        [args.readelf, "--notes", args.elf],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    build_ids = _BUILD_ID.findall(result.stdout)
    if len(build_ids) != 1:
        parser.error(
            f"expected exactly one GNU build ID in {args.elf}, found {len(build_ids)}"
        )

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(build_ids[0].lower() + "\n", encoding="ascii")

    depfile = pathlib.Path(args.depfile)
    depfile.parent.mkdir(parents=True, exist_ok=True)
    depfile.write_text(f"{output}: {args.elf}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
