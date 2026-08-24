#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(
        description="Convert a linked hermetic ELF into a raw binary blob"
    )
    parser.add_argument("objcopy")
    parser.add_argument("elf")
    parser.add_argument("output")
    args = parser.parse_args()

    subprocess.run(
        [args.objcopy, "-O", "binary", args.elf, args.output], check=True
    )


if __name__ == "__main__":
    main()
