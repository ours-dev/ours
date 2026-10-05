#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Driver for toolchain_utils_action() actions (see c_utils.gni).

Usage: toolchain_utils_action.py OUTPUT DEPFILE PROGRAM ARG...

Runs PROGRAM ARG... with special handling:
  * Every "@RSPFILE" argument is scanned: the rspfile path itself plus each
    non-switch (not starting with "-") line inside it are recorded as
    inputs in DEPFILE (target OUTPUT).
  * For tools known to lack built-in @rspfile support (currently only
    "ln"), the rspfile contents are spliced into the argument list instead
    of passing the @rspfile token.
On any failure the (partial) OUTPUT and DEPFILE are removed so Ninja does
not treat the action as succeeded.

NOTE(ours): this driver is Python because the repo sets
script_executable = "python3" in .gn, so actions always run through python3;
it is a faithful port of a bash original.
"""

import os
import subprocess
import sys


def main(argv):
    if len(argv) < 4:
        print("usage: toolchain_utils_action.py OUTPUT DEPFILE PROGRAM ARG...",
              file=sys.stderr)
        return 2
    output, depfile, program = argv[0], argv[1], argv[2]
    args = list(argv[3:])

    # Only the ln utility is known to lack @rspfile support.
    expand_rspfile = os.path.basename(program) == "ln"

    inputs = []
    final_args = []

    def read_rspfile(rspfile):
        """Record rspfile inputs; optionally splice contents into args."""
        with open(rspfile) as f:
            for line in f:
                file = line.strip()
                if not file:
                    continue
                if not file.startswith("-"):
                    inputs.append(file)
                if expand_rspfile:
                    final_args.append(file)

    for arg in args:
        if arg.startswith("@"):
            rspfile = arg[1:]
            inputs.append(rspfile)
            read_rspfile(rspfile)
            if expand_rspfile:
                # Contents were spliced above; drop the @rspfile token.
                continue
        final_args.append(arg)

    try:
        with open(depfile, "w") as f:
            f.write("%s: %s\n" % (output, " ".join(inputs)))
        return subprocess.call([program] + final_args)
    except BaseException:
        # Clean up partial outputs on failure so Ninja retries the action.
        for p in (output, depfile):
            try:
                os.unlink(p)
            except OSError:
                pass
        raise


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
