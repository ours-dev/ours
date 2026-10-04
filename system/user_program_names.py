#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Write the header of a user_program_names() target.

GN gathers each named program's install name from its `<name>.install_name`
target into JSON (generated_file with output_conversion = "json"); this turns
that into one constexpr std::string_view per program, plus DIRECTORY, the
directory programs install to unless they say otherwise. See
//system/user_program_names.gni for why the names come from the build.

Standard library only: the host's python3 is 3.9, with nothing installed
beside it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

IDENTIFIER = re.compile(r"[A-Z_][A-Z0-9_]*")

# Taken by the header itself, so no program may be named it.
DIRECTORY = "DIRECTORY"


def constant(name: str) -> str:
    """The constant for target |name|: in capitals, as the tree spells a
    constexpr, with the characters GN allows in a name and C++ does not
    turned into underscores."""
    return re.sub(r"[-.+]", "_", name).upper()


def quoted(text: str, what: str, owner: str) -> str:
    if '"' in text or "\\" in text:
        sys.exit(f"{owner}: {what} '{text}' needs escaping")
    return f'"{text}"'


def render(entries: list[dict], directory: str, owner: str) -> str:
    """The header text for |entries|, or SystemExit on a bad entry."""
    seen: dict[str, str] = {DIRECTORY: "the header's own DIRECTORY"}
    lines = []
    for entry in sorted(entries, key=lambda item: item["name"]):
        name, path, label = entry["name"], entry["path"], entry["label"]
        ident = constant(name)
        if not IDENTIFIER.fullmatch(ident):
            sys.exit(f"{owner}: {label}: target name '{name}' makes no C++ identifier")
        if ident in seen:
            sys.exit(f"{owner}: {label} and {seen[ident]} both make {ident}")
        seen[ident] = label
        lines.append(f"/// {label}\n")
        lines.append(
            f"inline constexpr std::string_view {ident}"
            f"{{{quoted(path, f'{label}: install name', owner)}}};\n"
        )
    lines.append("/// Where a program installs unless it names a place of its own.\n")
    lines.append(
        f"inline constexpr std::string_view {DIRECTORY}"
        f"{{{quoted(directory, 'directory', owner)}}};\n"
    )

    return (
        f"// Generated from {owner} by //system/user_program_names.py. Do not edit.\n"
        "\n"
        "#pragma once\n"
        "\n"
        "#include <string_view>\n"
        "\n"
        "/// The BootFS names of the programs this program starts, as the build\n"
        "/// installed them.\n"
        "namespace user_programs {\n"
        + "".join(lines)
        + "} // namespace user_programs\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", type=pathlib.Path, help="generated_file() output")
    parser.add_argument("header", type=pathlib.Path, help="header to write")
    parser.add_argument("owner", help="the user_program_names() label, for messages")
    parser.add_argument("--directory", required=True, help="user_program_dir")
    args = parser.parse_args()

    entries = json.loads(args.json.read_text())
    args.header.write_text(render(entries, args.directory, args.owner))
    return 0


if __name__ == "__main__":
    sys.exit(main())
