#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Emulation of `rm -f out && cp -af` that preserves nanosecond mtimes
# (needed on macOS where `cp` truncates them; see copy_command.sh).
#
# PROVENANCE: adapted from Fuchsia's //build/toolchain/copy.py
# (Copyright 2020 The Fuchsia Authors, BSD-style license).  Only the
# interpreter shebang and this header differ.
"""Emulation of `rm -f out && cp -af` in out. This is necessary on Mac in order
to preserve nanoseconds of mtime. See https://fxbug.dev/42134108#c5."""

import os
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: copy.py source dest", file=sys.stderr)
        return 1
    source = sys.argv[1]
    dest = sys.argv[2]

    if os.path.isdir(source):
        print(
            f'{source} is a directory, tool "copy" does not support directory copies'
        )
        return 1

    if os.path.exists(dest):
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        else:
            os.unlink(dest)

    shutil.copy2(source, dest)


if __name__ == "__main__":
    main()
