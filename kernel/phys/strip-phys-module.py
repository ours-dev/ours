#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Strip a phys module for packaging.

Debug information only.  Unlike a kernel image, a module keeps its section
headers: physload reads .phys.module and .init_array out of the section
table to find the entry the module declared and the constructors to run
(kernel/phys/module-loader.cpp).
"""

import argparse
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parsed = parser.parse_args()

    output = pathlib.Path(parsed.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [parsed.objcopy, "--strip-debug", parsed.input, parsed.output], check=True
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
