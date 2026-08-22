#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Materializes an expanded distribution manifest into a directory tree
# under the build dir: every {destination, source} entry becomes a copy of
# its source file at output/<destination>.  A depfile records every input so
# Ninja can rebuild when any referenced file changes.
#
# PROVENANCE: adapted from Fuchsia's
#   //build/dist/distribution-manifest-copy.py
# (Copyright 2024 The Fuchsia Authors, BSD-style license).  Only the
# interpreter shebang and this header differ from the original.


import argparse
import json
import shutil
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest",
        type=Path,
        help="JSON manifest file",
        required=True,
    )
    parser.add_argument(
        "--output",
        help="Output directory",
        type=Path,
        required=True,
    )
    parser.add_argument(
        "--depfile",
        type=Path,
        help="Depfile",
        required=True,
    )
    args = parser.parse_args()

    with args.manifest.open() as f:
        files = {
            args.output.joinpath(entry["destination"]): Path(entry["source"])
            for entry in json.load(f)
        }

    # Always completely remove any old directory.
    if args.output.exists():
        shutil.rmtree(args.output)

    # Create a fresh new directory, even if it will be empty.
    args.output.mkdir(parents=True, exist_ok=False)

    for dst, src in files.items():
        # Create any intermediate subdirectories needed.
        dst.parent.mkdir(parents=True, exist_ok=True)
        # Use an actual copy rather than a hard link to avoid strange anomalies
        # sometimes observed on bots when using hard links.
        shutil.copy2(src, dst)

    inputs = " ".join([str(f) for f in files.values()] + [str(args.manifest)])
    args.depfile.write_text(f"{args.output}: {inputs}\n")


if __name__ == "__main__":
    main()
