#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Check that the vDSO exports exactly the ABI the description promises.

Two .ifs files are compared: libours.ifs, generated from
//system/public/ours/syscalls.gni, and the one llvm-ifs reads out of the
linked vDSO. They must name the same SONAME and the same symbols with the
same types. A symbol the description has and the vDSO lacks is a call a
program can declare and never bind; one the vDSO has and the description
lacks is a door into the kernel nobody wrote down.

.ifs is YAML, which the standard library cannot parse, but llvm-ifs writes
one symbol per line in a fixed flow style, and generated_ifs_file() copies
that style on purpose so that the two can be compared as text. The lines
are read with a regular expression for that reason, and anything that does
not match is refused rather than skipped. Each symbol is compared on every
field its line carries.

Zircon counterpart: the verify-abi action in
//zircon/kernel/lib/userabi/vdso/BUILD.gn, which compares the .ifs extracted
from libzircon.so against //src/zircon/lib/zircon/zircon.ifs. That file is
maintained by hand; libours.ifs is generated from the same description as the
stubs, so what this catches is a stub the macro failed to emit or a symbol
exported by accident, rather than two hand-written lists drifting apart.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import List, Optional, Tuple

SONAME = re.compile(r"^SoName:\s*(\S+)\s*$")
SYMBOL = re.compile(r"^\s*-\s*\{(.*)\}\s*$")
FIELD = re.compile(r"^\s*([A-Za-z]+)\s*:\s*(\S+)\s*$")

# A symbol is compared on every field its line carries, not just Name and Type:
# `Undefined: true` on an ours_* would be a call the vDSO imports rather than
# defines, and `Weak: true` one a program's own definition could override --
# either would bind a program to something other than the kernel's stub.
Symbol = Tuple[Tuple[str, str], ...]


class IfsError(Exception):
    pass


def parse_symbol(path: pathlib.Path, number: int, body: str) -> Symbol:
    fields = {}
    for part in body.split(","):
        match = FIELD.match(part)
        if not match:
            raise IfsError(f"{path}:{number}: not a symbol field: {part!r}")
        fields[match.group(1)] = match.group(2)
    if "Name" not in fields or "Type" not in fields:
        raise IfsError(f"{path}:{number}: a symbol needs Name and Type")
    return tuple(sorted(fields.items()))


def describe(symbol: Symbol) -> str:
    fields = dict(symbol)
    rest = ", ".join(f"{k}: {v}" for k, v in symbol if k != "Name")
    return f"{fields['Name']} ({rest})"


def read_ifs(path: pathlib.Path) -> Tuple[Optional[str], List[Symbol]]:
    soname = None
    symbols = []
    in_symbols = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = SONAME.match(line)
        if match:
            soname = match.group(1)
            continue
        if line.startswith("Symbols:"):
            in_symbols = True
            continue
        if not in_symbols:
            continue
        if line.startswith("...") or (line and not line.startswith(" ")):
            in_symbols = False
            continue
        match = SYMBOL.match(line)
        if not match:
            raise IfsError(f"{path}:{number}: not a symbol line: {line!r}")
        symbols.append(parse_symbol(path, number, match.group(1)))
    return soname, sorted(symbols)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", type=pathlib.Path, help="libours.ifs, from the description")
    parser.add_argument("actual", type=pathlib.Path, help="the vDSO's exports, from llvm-ifs")
    parser.add_argument("stamp", type=pathlib.Path, help="written when the two agree")
    args = parser.parse_args()

    try:
        expected_soname, expected = read_ifs(args.expected)
        actual_soname, actual = read_ifs(args.actual)
    except IfsError as error:
        print(error, file=sys.stderr)
        return 1

    problems = []
    if expected_soname != actual_soname:
        problems.append(f"SONAME: described {expected_soname}, linked {actual_soname}")
    for symbol in sorted(set(expected) - set(actual)):
        problems.append(f"described but not exported: {describe(symbol)}")
    for symbol in sorted(set(actual) - set(expected)):
        problems.append(f"exported but not described: {describe(symbol)}")
    if problems:
        print(f"the vDSO does not match {args.expected}:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
