#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Write the published-record table that travels with a vDSO.

The kernel package carries the stripped vDSO, which has no .symtab. This
reads the unstripped ELF with llvm-readelf and writes one line per
published symbol: name, st_value, size. lay_out_vdso() turns st_value
into a VMO offset by subtracting the image base.

argv, after this script:
  [1] llvm-readelf (absolute; never PATH)
  [2] "section:.vdso_data", or a comma-separated allow-list of names
  [3] unstripped ELF
  [4] output path

section: collects every object symbol in that section. That is what
VDSO_EXPORT produces. An allow-list still refuses a name that is missing
or that does not live in allocated read-only data.
"""

import json
import subprocess
import sys
from typing import Optional

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4


def _die(message: str) -> None:
    print(f"gen_published_table: {message}", file=sys.stderr)
    sys.exit(1)


def _walk(node, symbols: list, sections: dict) -> None:
    if isinstance(node, dict):
        symbol = node.get("Symbol")
        if isinstance(symbol, dict) and "Value" in symbol and "Size" in symbol:
            symbols.append(symbol)
        section = node.get("Section")
        if isinstance(section, dict) and "Flags" in section:
            name = section.get("Name")
            if isinstance(name, dict):
                name = name.get("Name")
            flags = section.get("Flags")
            if isinstance(name, str) and isinstance(flags, dict) and "Value" in flags:
                sections[name] = int(flags["Value"])
        for value in node.values():
            _walk(value, symbols, sections)
    elif isinstance(node, list):
        for item in node:
            _walk(item, symbols, sections)


def _name_of(symbol: dict) -> Optional[str]:
    name = symbol.get("Name")
    if isinstance(name, dict):
        name = name.get("Name")
    if isinstance(name, str) and name:
        return name
    return None


def _type_name(symbol: dict) -> Optional[str]:
    kind = symbol.get("Type")
    if isinstance(kind, dict):
        kind = kind.get("Name")
    if isinstance(kind, str):
        return kind
    return None


def _section_name(symbol: dict) -> Optional[str]:
    section = symbol.get("Section")
    if isinstance(section, dict):
        name = section.get("Name")
        if isinstance(name, dict):
            name = name.get("Name")
        if isinstance(name, str):
            return name
    return None


def main() -> None:
    if len(sys.argv) != 5:
        _die(
            "usage: gen_published_table.py llvm-readelf names elf output"
        )
    readelf, names_arg, elf, output = sys.argv[1:]
    section_filter = None
    if names_arg.startswith("section:"):
        section_filter = names_arg[len("section:") :]
        if not section_filter:
            _die("section: needs a section name")
        names = []
    else:
        names = [name for name in names_arg.split(",") if name]
        if not names:
            _die("the allow-list is empty")

    completed = subprocess.run(
        [readelf, "--elf-output-style=JSON", "--symbols", "--sections", elf],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        _die(completed.stderr.strip() or f"llvm-readelf failed on {elf}")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        _die(f"llvm-readelf JSON: {error}")

    symbols: list = []
    sections: dict = {}
    _walk(payload, symbols, sections)

    lines = ["# name st_value size", ""]
    if section_filter is not None:
        names = []
        for symbol in symbols:
            if _section_name(symbol) != section_filter:
                continue
            if _type_name(symbol) != "Object":
                continue
            name = _name_of(symbol)
            if name is None or name.startswith("$"):
                continue
            names.append(name)
        names = sorted(set(names))
        if not names:
            _die(f"{elf}: no object symbols in {section_filter}")
    for name in names:
        matches = [symbol for symbol in symbols if _name_of(symbol) == name]
        if not matches:
            _die(f"{elf}: no symbol {name}")
        chosen = matches[0]
        value = int(chosen["Value"])
        size = int(chosen["Size"])
        for other in matches[1:]:
            if int(other["Value"]) != value or int(other["Size"]) != size:
                _die(f"{elf}: {name} has disagreeing symbol-table entries")
        section = _section_name(chosen)
        if section is None or section not in sections:
            _die(f"{elf}: {name} has no section")
        flags = sections[section]
        if (flags & SHF_ALLOC) == 0 or (flags & SHF_WRITE) != 0 or (
            flags & SHF_EXECINSTR
        ) != 0:
            _die(
                f"{elf}: {name} is in {section}, which is not allocated read-only data"
            )
        if value % 8 != 0:
            _die(f"{elf}: {name} st_value {value:#x} is not 8-byte aligned")
        lines.append(f"{name} {value:#x} {size}")

    with open(output, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
