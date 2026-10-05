#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Converts an expanded distribution manifest (JSON) to the FINI (INI) text
# format: one "destination=source" line per entry, sorted by destination.
#
# PROVENANCE: adapted from Fuchsia's
#   //build/dist/convert_manifest_to_fini.py
# (Copyright 2020 The Fuchsia Authors, BSD-style license).  Only the
# interpreter shebang, this header and the module docstring differ from the
# original.

"""Converts a distribution manifest to the FINI text format."""

import argparse
import json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input", help="Path to original manifest", required=True
    )
    parser.add_argument(
        "--output", help="Path to the formatted manifest", required=True
    )
    parser.add_argument(
        "--prefix",
        help="Prefix prepended verbatim to destination paths",
        default="",
    )
    args = parser.parse_args()

    with open(args.input, "r") as input_file:
        objects = json.load(input_file)
    lines = sorted(
        args.prefix + o["destination"] + "=" + o["source"] + "\n"
        for o in objects
    )
    with open(args.output, "w") as output_file:
        output_file.writelines(lines)


if __name__ == "__main__":
    main()
