#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Read the root's built-in capabilities from their data file.

The built-ins are what `from: "parent"` means at the root: the directory
`boot` and the protocols the root serves itself. One data file lists them
(system/service/init/builtins.json), so that the manifest compiler, which
checks a route that ends at one, and the root, which routes to it, read the
same list and cannot disagree:

  {
    "builtins": [
      {"name": "boot", "kind": "directory", "rights": ["rx*"]},
      {"name": "ours.process.Launcher", "kind": "protocol"},
      {"name": "ours.resource.Irq", "kind": "protocol", "type": "ours.resource.Irq"}
    ]
  }

`name` and `kind` ("protocol" or "directory") are required. `type`, the
protocol a protocol built-in speaks, is optional and is the name when omitted;
a directory has none. `rights`, a directory's, uses the words of a manifest and
is allowed on a directory only.

The compiler calls load(). The command line writes the C++ include file the
root defines its `BUILTINS` list from, in the order of the data file:

  component_builtins.py --input builtins.json --output builtins.inc

Standard library only: the host's python3 is 3.9, with nothing installed
beside it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any, Dict, List, Optional

import component_manifest as cm

KINDS = {"protocol": cm.KIND_PROTOCOL, "directory": cm.KIND_DIRECTORY}

# What the include file spells a kind and a mask of rights as: the library's
# enumerators for the two aliases a manifest uses, the mask itself otherwise.
_KIND_ENUMERATORS = {cm.KIND_PROTOCOL: "Protocol", cm.KIND_DIRECTORY: "Directory"}
_RIGHTS_ENUMERATORS = {cm.RIGHTS["r*"]: "RStarDir", cm.RIGHTS["rx*"]: "RxStarDir"}


class BuiltinsError(cm.ManifestError):
    """A data file the build refuses: where in it, and why."""


@dataclass
class Builtin:
    """One built-in capability, as component::Builtin holds it."""

    name: str
    kind: int
    # The protocol a protocol built-in speaks: stated, or the name. A
    # directory speaks none, and its type is its name, which nothing compares.
    type: str
    # A directory's rights as a mask; None when none are stated, always None
    # for a protocol.
    rights: Optional[int]


def _name(value: Any, where: str) -> str:
    """|value| as a name, as the manifest compiler reads one."""
    try:
        return cm.check_name(value, where)
    except cm.ManifestError as error:
        raise BuiltinsError(str(error)) from None


def _entry(clause: Any, where: str) -> Builtin:
    if not isinstance(clause, dict):
        raise BuiltinsError(f"{where}: expected a scope")
    unknown = sorted(set(clause) - {"name", "kind", "type", "rights"})
    if unknown:
        raise BuiltinsError(
            f"{where}: unknown key '{unknown[0]}'; allowed here: name, kind, type, rights"
        )
    for key in ("name", "kind"):
        if key not in clause:
            raise BuiltinsError(f"{where}: needs `{key}`")

    name = _name(clause["name"], f"{where}.name")
    kind_word = clause["kind"]
    if not isinstance(kind_word, str) or kind_word not in KINDS:
        raise BuiltinsError(
            f"{where}.kind: {json.dumps(kind_word)} is not one of: {', '.join(sorted(KINDS))}"
        )
    kind = KINDS[kind_word]

    if kind == cm.KIND_DIRECTORY:
        if "type" in clause:
            raise BuiltinsError(
                f"{where}.type: a directory has no `type`; only a protocol has one, and a "
                "directory's hops are compared by their rights"
            )
        rights = None
        if "rights" in clause:
            try:
                rights = cm.check_rights(clause["rights"], f"{where}.rights")
            except cm.ManifestError as error:
                raise BuiltinsError(str(error)) from None
        return Builtin(name, kind, name, rights)

    if "rights" in clause:
        raise BuiltinsError(f"{where}.rights: only a directory has `rights`")
    type_ = _name(clause["type"], f"{where}.type") if "type" in clause else name
    return Builtin(name, kind, type_, None)


def load(path: str) -> List[Builtin]:
    """The entries of the data file at |path|, in its order. Raises
    BuiltinsError on the first rule broken, naming the file."""
    try:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    except OSError as error:
        raise BuiltinsError(f"{path}: cannot be read: {error.strerror}") from None
    except json.JSONDecodeError as error:
        raise BuiltinsError(f"{path}: is not JSON: {error}") from None

    where = path
    if not isinstance(data, dict):
        raise BuiltinsError(f"{where}: expected a scope with a `builtins` list")
    unknown = sorted(set(data) - {"builtins"})
    if unknown:
        raise BuiltinsError(f"{where}: unknown key '{unknown[0]}'; allowed here: builtins")
    entries = data.get("builtins")
    if not isinstance(entries, list):
        raise BuiltinsError(f"{where}: needs a `builtins` list")

    builtins: List[Builtin] = []
    seen: Dict[str, int] = {}
    for index, clause in enumerate(entries):
        at = f"{where}: builtins[{index}]"
        builtin = _entry(clause, at)
        # One name, whatever its kind: the root finds a built-in by its name
        # (a protocol under /svc, the directory at the root of the namespace),
        # so a protocol and a directory of one name would be two answers to
        # one question.
        if builtin.name in seen:
            raise BuiltinsError(
                f"{at}.name: '{builtin.name}' is a duplicate built-in "
                f"(builtins[{seen[builtin.name]}])"
            )
        seen[builtin.name] = index
        builtins.append(builtin)
    return builtins


def _rights_text(rights: Optional[int]) -> str:
    if rights is None:
        return "ustl::NONE"
    if rights in _RIGHTS_ENUMERATORS:
        value = f"component::Rights::{_RIGHTS_ENUMERATORS[rights]}"
    else:
        value = f"static_cast<component::Rights>({rights:#x})"
    return f"ustl::Option<component::Rights>({value})"


def render(builtins: List[Builtin], source: str) -> str:
    """The include file for |builtins|, which came from the file |source|:
    the definition of `BUILTINS`, with every name spelled out in full so that
    it can be included wherever the list is wanted. The including file brings
    <array>, <component/route.hpp> and <ustl/option.hpp>; this file has no
    include of its own, since it may sit inside a namespace."""
    lines = [
        f"// Generated from {source} by system/component_builtins.py. Do not edit.",
        "//",
        "// The root's built-in capabilities, in the order of that file. Include it",
        "// where `BUILTINS` is to be defined, after <array>, <component/route.hpp>",
        "// and <ustl/option.hpp>.",
    ]
    if not builtins:
        # std::array{} of nothing has no type to deduce.
        lines.append("constexpr auto BUILTINS = std::array<component::Builtin, 0>{};")
        return "\n".join(lines) + "\n"
    lines.append("constexpr auto BUILTINS = std::array{")
    for builtin in builtins:
        lines += [
            "    component::Builtin{",
            f"        .kind = component::Kind::{_KIND_ENUMERATORS[builtin.kind]},",
            f'        .name = "{builtin.name}",',
            f"        .rights = {_rights_text(builtin.rights)},",
            f'        .type = "{builtin.type}",',
            "    },",
        ]
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, help="the built-ins' data file, as JSON")
    parser.add_argument("--output", required=True, help="the C++ include file to write")
    args = parser.parse_args(argv)

    try:
        builtins = load(args.input)
    except cm.ManifestError as error:
        print(f"component_builtins: {error}", file=sys.stderr)
        return 1

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(render(builtins, args.input))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
