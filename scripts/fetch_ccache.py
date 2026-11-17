#!/usr/bin/env python3
"""Fetch the pinned ccache release into prebuilt/ccache/.

    python3 scripts/fetch_ccache.py [--force]

ccache is the compiler cache the build can put in front of every compile, so
that a source one worktree has compiled is not compiled again in another. Its
release and hash are pinned in build/prebuilt.json with the rest of the
toolchain, and scripts/fetch_prebuilt.py fetches it; this is that script
limited to ccache, kept under the name the verify skill gives.

A checkout under .worktrees/ links prebuilt/ to the project root's, so one
fetch serves every worktree. Does nothing when prebuilt/ccache/ already holds
the pinned release, or holds a tree this script did not make, unless --force
is given.
"""
from __future__ import annotations

import argparse
import sys

import fetch_prebuilt


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true",
                        help="download and unpack again even if a ccache is in place")
    args = parser.parse_args(argv)
    return fetch_prebuilt.main(["--only", "ccache", *(["--force"] if args.force else [])])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
