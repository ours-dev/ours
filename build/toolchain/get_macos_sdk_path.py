#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Prints the active macOS SDK path (`xcrun --sdk macosx --show-sdk-path`),
# or nothing on failure. Only invoked when host_os == "mac"; the prebuilt
# Clang used there has no baked-in default sysroot, unlike the container's
# apt-installed llvm-22, which resolves headers against the Linux system
# root it was installed into.

import subprocess
import sys


def main() -> int:
    try:
        result = subprocess.run(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return 0
    print(result.stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
