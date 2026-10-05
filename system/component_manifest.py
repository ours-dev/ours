#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Compile one component_manifest() target into its binary manifest.

The manifest compiler, for the manifest language of
docs/design/261004/component-framework.md (D3). GN writes the target's scope
out as JSON, together with the program's install name and the meta path of
each child (//system/component_manifest.gni); this checks them against the
rules one manifest must keep on its own and writes `meta/<name>.cm` in the
flat format that //system/lib/component decodes (D2).

The rules run here, once, at build time. The decoder in the root checks only
the structure, because the build alone writes the BootFS the root reads its
manifests from: a manifest that reached the root passed them already.

Standard library only: the host's python3 is 3.9, with nothing installed
beside it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import sys
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

# The compiled format. //system/lib/component/include/component/format.hpp is
# the other half of this contract: every constant and every layout below has
# its twin there, and the decoder's test decodes files this script wrote.

# "OSCM" as the first four bytes of the file, read as a little-endian u32.
MAGIC = 0x4D43534F
VERSION = 1

# How long a name may be: a child's name, a capability's, an `as`. A protocol
# name of at most 100 bytes always fits the one path segment it becomes under
# /svc (MAX_SEGMENT_LENGTH).
MAX_NAME_LENGTH = 100

# A path or subdir: the longest path an ours.io Open carries
# (ours_io::MAX_PATH_LENGTH).
MAX_PATH_LENGTH = 1024

# One segment of a path: the longest name of an ours.io directory entry
# (ours_io::MAX_NAME_LENGTH).
MAX_SEGMENT_LENGTH = 255

# What a child's URL is made of (D26): the boot resolver's spelling with /boot
# as the package directory, so the path part is empty and the fragment is the
# manifest's place in the BootFS.
BOOT_URL_PREFIX = "fuchsia-boot:///#"

# Where every program lives in the BootFS (S3: the root makes files under it
# executable at ingest, and nothing else).
PROGRAM_DIRECTORY = "bin/"

KIND_PROTOCOL = 1
KIND_DIRECTORY = 2

REF_PARENT = 1
REF_SELF = 2
REF_CHILD = 3

RUNNER_ELF = 1

# StartupMode Eager, the only one: lazy start is cut (D4).
STARTUP_EAGER = 1

# A record's flags word. RIGHTS_STATED tells "this hop states no rights"
# apart from "this hop states an empty set".
FLAG_RIGHTS_STATED = 0x1

# The rights a directory hop states, by their ours.io wire values
# (ours_io::Operations), under the words a manifest spells them with.
_CONNECT = 0x01
_READ_BYTES = 0x02
_WRITE_BYTES = 0x04
_EXECUTE = 0x08
_GET_ATTRIBUTES = 0x10
_UPDATE_ATTRIBUTES = 0x20
_ENUMERATE = 0x40
_TRAVERSE = 0x80
_MODIFY_DIRECTORY = 0x100

RIGHTS = {
    "connect": _CONNECT,
    "enumerate": _ENUMERATE,
    "execute": _EXECUTE,
    "get_attributes": _GET_ATTRIBUTES,
    "modify_directory": _MODIFY_DIRECTORY,
    "read_bytes": _READ_BYTES,
    "traverse": _TRAVERSE,
    "update_attributes": _UPDATE_ATTRIBUTES,
    "write_bytes": _WRITE_BYTES,
    # The aliases, as ours_io's R_STAR_DIR and the rest define them.
    "r*": _CONNECT | _ENUMERATE | _TRAVERSE | _READ_BYTES | _GET_ATTRIBUTES,
    "w*": _CONNECT
    | _ENUMERATE
    | _TRAVERSE
    | _WRITE_BYTES
    | _MODIFY_DIRECTORY
    | _UPDATE_ATTRIBUTES,
    "x*": _CONNECT | _ENUMERATE | _TRAVERSE | _EXECUTE,
    "rw*": _CONNECT
    | _ENUMERATE
    | _TRAVERSE
    | _READ_BYTES
    | _WRITE_BYTES
    | _MODIFY_DIRECTORY
    | _GET_ATTRIBUTES
    | _UPDATE_ATTRIBUTES,
    "rx*": _CONNECT | _ENUMERATE | _TRAVERSE | _READ_BYTES | _GET_ATTRIBUTES | _EXECUTE,
}

# Little-endian, no padding: "<" packs exactly the fields named, and each
# layout is laid out so that every u64 already falls on a multiple of 8.
HEADER = struct.Struct("<IHHIIIIIIIIII")  # 48 bytes
PROGRAM = struct.Struct("<IIII")  # runner, reserved, binary
ARG = struct.Struct("<II")  # value
CHILD = struct.Struct("<IIIIII")  # name, url, startup, reserved
CAPABILITY = struct.Struct("<IIII")  # kind, reserved, name
USE = struct.Struct("<IIIIIIIIIIQ")  # 48 bytes
OFFER = struct.Struct("<IIIIIIIIIIIIQ")  # 56 bytes
EXPOSE = struct.Struct("<IIIIIIIIII")  # 40 bytes

# A name: a letter, digit or underscore, then those and '-' and '.', so that
# it is one path segment and never '.' or '..'.
_NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.\-]*")


class ManifestError(Exception):
    """A manifest the build refuses: where in it, and why."""


@dataclass
class Ref:
    kind: int
    child: str = ""


@dataclass
class Program:
    binary: str
    args: List[str]


@dataclass
class Child:
    name: str
    url: str


@dataclass
class Capability:
    kind: int
    name: str


@dataclass
class Use:
    kind: int
    source_name: str
    target_path: str
    subdir: str
    rights: Optional[int]


@dataclass
class Offer:
    kind: int
    source: Ref
    source_name: str
    target: Ref
    target_name: str
    subdir: str
    rights: Optional[int]


@dataclass
class Expose:
    kind: int
    source: Ref
    source_name: str
    target_name: str


@dataclass
class Decl:
    program: Optional[Program] = None
    children: List[Child] = field(default_factory=list)
    capabilities: List[Capability] = field(default_factory=list)
    uses: List[Use] = field(default_factory=list)
    offers: List[Offer] = field(default_factory=list)
    exposes: List[Expose] = field(default_factory=list)


def _string(value: Any, where: str) -> str:
    if not isinstance(value, str):
        raise ManifestError(f"{where}: expected a string, got {json.dumps(value)}")
    if "\0" in value:
        raise ManifestError(f"{where}: '{value}' contains a NUL byte")
    return value


def check_name(value: Any, where: str) -> str:
    """|value| as a name: _NAME, at most MAX_NAME_LENGTH bytes."""
    name = _string(value, where)
    if not name:
        raise ManifestError(f"{where}: a name must not be empty")
    if len(name.encode()) > MAX_NAME_LENGTH:
        raise ManifestError(f"{where}: '{name}' is longer than {MAX_NAME_LENGTH} bytes")
    if not _NAME.fullmatch(name):
        raise ManifestError(
            f"{where}: '{name}' is not a name: [A-Za-z0-9_] then [A-Za-z0-9_.-]"
        )
    return name


def _check_segments(path: str, segments: List[str], where: str) -> None:
    for segment in segments:
        if segment in ("", ".", ".."):
            raise ManifestError(f"{where}: '{path}' has an empty, '.' or '..' segment")
        if len(segment.encode()) > MAX_SEGMENT_LENGTH:
            raise ManifestError(
                f"{where}: '{path}' has a segment longer than {MAX_SEGMENT_LENGTH} bytes"
            )


def check_path(value: Any, where: str) -> str:
    """|value| as an absolute namespace path: a leading '/', no trailing one,
    and no empty, '.' or '..' segment, so that a place has one spelling."""
    path = _string(value, where)
    if not path.startswith("/") or path == "/":
        raise ManifestError(f"{where}: '{path}' is not an absolute path below '/'")
    if len(path.encode()) > MAX_PATH_LENGTH:
        raise ManifestError(f"{where}: '{path}' is longer than {MAX_PATH_LENGTH} bytes")
    _check_segments(path, path[1:].split("/"), where)
    return path


def check_subdir(value: Any, where: str) -> str:
    """|value| as a subdirectory: a relative path, with the segments
    check_path() allows."""
    path = _string(value, where)
    if path.startswith("/"):
        raise ManifestError(f"{where}: subdir '{path}' must be relative")
    if len(path.encode()) > MAX_PATH_LENGTH:
        raise ManifestError(f"{where}: '{path}' is longer than {MAX_PATH_LENGTH} bytes")
    _check_segments(path, path.split("/"), where)
    return path


def check_rights(value: Any, where: str) -> int:
    """|value| as a non-empty list of distinct rights words, as one mask."""
    if not isinstance(value, list) or not value:
        raise ManifestError(f"{where}: rights must be a non-empty list of words")
    rights = 0
    seen = set()
    for word in value:
        word = _string(word, where)
        if word not in RIGHTS:
            raise ManifestError(
                f"{where}: unknown right '{word}'; known: {', '.join(sorted(RIGHTS))}"
            )
        if word in seen:
            raise ManifestError(f"{where}: '{word}' is duplicated in the rights clause")
        seen.add(word)
        rights |= RIGHTS[word]
    return rights


def _check_keys(clause: Any, allowed: Tuple[str, ...], where: str) -> Dict[str, Any]:
    if not isinstance(clause, dict):
        raise ManifestError(f"{where}: expected a scope")
    unknown = sorted(set(clause) - set(allowed))
    if unknown:
        raise ManifestError(
            f"{where}: unknown key '{unknown[0]}'; allowed here: {', '.join(allowed)}"
        )
    return clause


def _list(clause: Dict[str, Any], key: str, where: str) -> List[Any]:
    value = clause.get(key, [])
    if not isinstance(value, list):
        raise ManifestError(f"{where}: `{key}` must be a list")
    return value


def _one_or_many_names(value: Any, where: str) -> List[str]:
    """A name or a non-empty list of distinct names."""
    if isinstance(value, list):
        if not value:
            raise ManifestError(f"{where}: the list of names is empty")
        names = [check_name(item, where) for item in value]
        if len(set(names)) != len(names):
            raise ManifestError(f"{where}: a name appears twice")
        return names
    return [check_name(value, where)]


def _capability_names(clause: Dict[str, Any], where: str) -> Tuple[int, List[str]]:
    """Which kind of capability |clause| names, and the names."""
    has_protocol = "protocol" in clause
    has_directory = "directory" in clause
    if has_protocol == has_directory:
        raise ManifestError(f"{where}: needs exactly one of `protocol` and `directory`")
    if has_protocol:
        return KIND_PROTOCOL, _one_or_many_names(clause["protocol"], f"{where}.protocol")
    return KIND_DIRECTORY, _one_or_many_names(clause["directory"], f"{where}.directory")


def _child_ref(value: Any, children: Dict[str, int], where: str) -> str:
    text = _string(value, where)
    if not text.startswith("#"):
        raise ManifestError(f"{where}: '{text}' is not a child reference '#<name>'")
    name = text[1:]
    if name not in children:
        raise ManifestError(f"{where}: '{text}' names no child of this component")
    return name


def _source_ref(
    value: Any,
    allowed: Tuple[str, ...],
    children: Dict[str, int],
    where: str,
) -> Ref:
    text = _string(value, where)
    if text == "parent" and "parent" in allowed:
        return Ref(REF_PARENT)
    if text == "self" and "self" in allowed:
        return Ref(REF_SELF)
    if text.startswith("#") and "#child" in allowed:
        return Ref(REF_CHILD, _child_ref(text, children, where))
    raise ManifestError(f"{where}: '{text}' is not one of: {', '.join(allowed)}")


def _plain_label(value: Any, where: str) -> str:
    """|value| as a label with no toolchain. The template adds the user
    toolchain itself and asserts on a label that already names one; this
    holds the JSON it hands over to the same rule."""
    label = _string(value, where)
    if "(" in label:
        raise ManifestError(
            f"{where}: {label} names a toolchain; give a plain label, the template adds "
            "the user toolchain"
        )
    return label


def _program(
    source: Dict[str, Any], program_names: List[Dict[str, Any]], where: str
) -> Optional[Program]:
    manifest = source.get("manifest", {})
    if "program" not in source:
        if "program" in manifest:
            raise ManifestError(
                f"{where}.manifest.program: there is no program; name one with the "
                "template's `program`"
            )
        return None

    label = _plain_label(source["program"], f"{where}.program")
    if len(program_names) != 1:
        raise ManifestError(
            f"{where}: {label} gave {len(program_names)} install names; is it a "
            "user_executable()?"
        )
    binary = _string(program_names[0].get("path"), f"{where}: {label}'s install name")
    if not binary.startswith(PROGRAM_DIRECTORY) or binary == PROGRAM_DIRECTORY:
        raise ManifestError(
            f"{where}: {label} installs as '{binary}', not under '{PROGRAM_DIRECTORY}', "
            "where the root makes programs executable"
        )
    check_subdir(binary, f"{where}: {label}'s install name")

    clause = _check_keys(manifest.get("program", {}), ("args",), f"{where}.manifest.program")
    args = [
        _string(arg, f"{where}.manifest.program.args[{index}]")
        for index, arg in enumerate(_list(clause, "args", f"{where}.manifest.program"))
    ]
    return Program(binary, args)


def _children(
    source: Dict[str, Any], children_meta: List[Dict[str, Any]], where: str
) -> List[Child]:
    paths: Dict[str, str] = {}
    for entry in children_meta:
        paths[entry["label"]] = entry["path"]

    children = []
    seen = set()
    for index, clause in enumerate(_list(source, "children", where)):
        at = f"{where}.children[{index}]"
        clause = _check_keys(clause, ("name", "manifest", "startup"), at)
        if "name" not in clause:
            raise ManifestError(f"{at}: needs `name`")
        name = check_name(clause["name"], f"{at}.name")
        if name in seen:
            raise ManifestError(f"{at}.name: '{name}' is a duplicate child name")
        seen.add(name)

        # ours has only eager (D4), and the word must still be written, so
        # that a manifest says when its child starts whatever the default
        # becomes once lazy start is added.
        startup = clause.get("startup")
        if startup == "lazy":
            raise ManifestError(f"{at}.startup: 'lazy' is not supported; only 'eager' (D4)")
        if startup != "eager":
            raise ManifestError(f"{at}.startup: must be written out as \"eager\" (D4)")

        label = _plain_label(clause.get("manifest"), f"{at}.manifest")
        if label not in paths:
            raise ManifestError(f"{at}.manifest: {label} is not a component_manifest()")
        children.append(Child(name, BOOT_URL_PREFIX + paths[label]))
    return children


def _capabilities(manifest: Dict[str, Any], where: str) -> List[Capability]:
    capabilities = []
    seen = set()
    for index, clause in enumerate(_list(manifest, "capabilities", where)):
        at = f"{where}.capabilities[{index}]"
        if isinstance(clause, dict) and "directory" in clause:
            raise ManifestError(
                f"{at}: a component cannot declare a directory; the only one is the "
                "root's built-in `boot` (D3)"
            )
        clause = _check_keys(clause, ("protocol",), at)
        if "protocol" not in clause:
            raise ManifestError(f"{at}: needs `protocol`")
        for name in _one_or_many_names(clause["protocol"], f"{at}.protocol"):
            if name in seen:
                raise ManifestError(f"{at}: '{name}' is a duplicate capability name")
            seen.add(name)
            capabilities.append(Capability(KIND_PROTOCOL, name))
    return capabilities


def _segments(path: str) -> List[str]:
    return path[1:].split("/")


def _is_prefix(shorter: str, longer: str) -> bool:
    """Whether |shorter| is |longer| or a directory above it, by segments,
    so that /svc is not a prefix of /svcx."""
    a, b = _segments(shorter), _segments(longer)
    return len(a) <= len(b) and b[: len(a)] == a


def _uses(manifest: Dict[str, Any], where: str) -> List[Use]:
    uses = []
    paths: List[Tuple[str, str]] = []
    for index, clause in enumerate(_list(manifest, "use", where)):
        at = f"{where}.use[{index}]"
        clause = _check_keys(
            clause, ("protocol", "directory", "from", "path", "rights", "subdir"), at
        )
        kind, names = _capability_names(clause, at)
        # Every use comes from the parent (D3); `from` may say so.
        if clause.get("from", "parent") != "parent":
            raise ManifestError(f"{at}.from: a use comes from the parent only (D3)")

        if kind == KIND_PROTOCOL:
            for key in ("path", "rights", "subdir"):
                if key in clause:
                    # Every protocol use lives at /svc/<name> (D3), where the
                    # root serves one /svc per component.
                    raise ManifestError(
                        f"{at}.{key}: a protocol use has no `{key}`; it is always at "
                        "/svc/<name> (D3)"
                    )
            for name in names:
                uses.append(Use(kind, name, f"/svc/{name}", "", None))
                paths.append((f"/svc/{name}", f"{at} protocol '{name}'"))
            continue

        if len(names) != 1:
            raise ManifestError(f"{at}.directory: a directory use names one directory")
        if "path" not in clause or "rights" not in clause:
            raise ManifestError(f"{at}: a directory use needs `path` and `rights`")
        path = check_path(clause["path"], f"{at}.path")
        rights = check_rights(clause["rights"], f"{at}.rights")
        subdir = check_subdir(clause["subdir"], f"{at}.subdir") if "subdir" in clause else ""
        uses.append(Use(kind, names[0], path, subdir, rights))
        paths.append((path, f"{at} directory '{names[0]}'"))

    # Two namespace entries at one path, or one inside the other, cannot both
    # be bound: a binding inside another would shadow part of it (fsio's
    # Namespace refuses both).
    for first in range(len(paths)):
        for second in range(first + 1, len(paths)):
            (path_a, what_a), (path_b, what_b) = paths[first], paths[second]
            if path_a == path_b:
                raise ManifestError(f"{what_b}: '{path_b}' is a duplicate use path ({what_a})")
            if _is_prefix(path_a, path_b) or _is_prefix(path_b, path_a):
                raise ManifestError(
                    f"{what_b}: '{path_b}' and '{path_a}' of {what_a} are nested"
                )
    return uses


def _offers(
    manifest: Dict[str, Any],
    children: Dict[str, int],
    declared: Dict[str, int],
    where: str,
) -> List[Offer]:
    offers = []
    targets: Dict[Tuple[str, str], str] = {}
    for index, clause in enumerate(_list(manifest, "offer", where)):
        at = f"{where}.offer[{index}]"
        clause = _check_keys(
            clause, ("protocol", "directory", "from", "to", "as", "rights", "subdir"), at
        )
        kind, names = _capability_names(clause, at)
        if "from" not in clause or "to" not in clause:
            raise ManifestError(f"{at}: an offer needs `from` and `to`")

        # A directory can only come from the parent: no component provides
        # one, so `boot` from the root is the only source there is (D3).
        allowed: Tuple[str, ...] = ("parent", "self", "#child")
        if kind == KIND_DIRECTORY:
            allowed = ("parent",)
        source = _source_ref(clause["from"], allowed, children, f"{at}.from")
        if source.kind == REF_SELF:
            for name in names:
                if declared.get(name) != kind:
                    raise ManifestError(
                        f"{at}.from: 'self' offers '{name}', which `capabilities` does not "
                        "declare"
                    )

        if kind == KIND_PROTOCOL:
            for key in ("rights", "subdir"):
                if key in clause:
                    raise ManifestError(f"{at}.{key}: only a directory offer has `{key}`")
        rights = check_rights(clause["rights"], f"{at}.rights") if "rights" in clause else None
        subdir = check_subdir(clause["subdir"], f"{at}.subdir") if "subdir" in clause else ""

        if "as" in clause and len(names) != 1:
            raise ManifestError(f"{at}.as: `as` renames one capability, not {len(names)}")
        renamed = check_name(clause["as"], f"{at}.as") if "as" in clause else None

        raw_targets = clause["to"]
        if not isinstance(raw_targets, list):
            raw_targets = [raw_targets]
        if not raw_targets:
            raise ManifestError(f"{at}.to: the list of targets is empty")
        target_names = []
        for target_index, value in enumerate(raw_targets):
            target = _child_ref(value, children, f"{at}.to[{target_index}]")
            if target in target_names:
                raise ManifestError(f"{at}.to: '#{target}' appears twice")
            if source.kind == REF_CHILD and source.child == target:
                raise ManifestError(f"{at}: '#{target}' is offered its own capability")
            target_names.append(target)

        for target in target_names:
            for name in names:
                target_name = renamed if renamed is not None else name
                key = (target, target_name)
                if key in targets:
                    raise ManifestError(
                        f"{at}: '{target_name}' is a duplicate offer target capability for "
                        f"'#{target}' ({targets[key]})"
                    )
                targets[key] = at
                offers.append(
                    Offer(
                        kind,
                        source,
                        name,
                        Ref(REF_CHILD, target),
                        target_name,
                        subdir,
                        rights,
                    )
                )
    return offers


def _exposes(
    manifest: Dict[str, Any],
    children: Dict[str, int],
    declared: Dict[str, int],
    where: str,
) -> List[Expose]:
    exposes = []
    targets: Dict[str, str] = {}
    for index, clause in enumerate(_list(manifest, "expose", where)):
        at = f"{where}.expose[{index}]"
        if isinstance(clause, dict) and "directory" in clause:
            raise ManifestError(
                f"{at}: a component cannot expose a directory; the only one is the root's "
                "built-in `boot` (D3)"
            )
        clause = _check_keys(clause, ("protocol", "from", "as", "to"), at)
        kind, names = _capability_names(clause, at)
        if "from" not in clause:
            raise ManifestError(f"{at}: an expose needs `from`")
        source = _source_ref(clause["from"], ("self", "#child"), children, f"{at}.from")
        if source.kind == REF_SELF:
            for name in names:
                if declared.get(name) != kind:
                    raise ManifestError(
                        f"{at}.from: 'self' exposes '{name}', which `capabilities` does not "
                        "declare"
                    )
        if clause.get("to", "parent") != "parent":
            raise ManifestError(f"{at}.to: an expose goes to the parent only")
        if "as" in clause and len(names) != 1:
            raise ManifestError(f"{at}.as: `as` renames one capability, not {len(names)}")
        renamed = check_name(clause["as"], f"{at}.as") if "as" in clause else None

        for name in names:
            target_name = renamed if renamed is not None else name
            if target_name in targets:
                raise ManifestError(
                    f"{at}: '{target_name}' is a duplicate expose target capability for "
                    f"the parent ({targets[target_name]})"
                )
            targets[target_name] = at
            exposes.append(Expose(kind, source, name, target_name))
    return exposes


def _check_cycles(offers: List[Offer], children: List[Child], where: str) -> None:
    """Refuses offers between children that form a cycle.

    An offer from #a to #b makes b depend on a, so a must start first and
    stop last. A cycle leaves no order to start them in.
    """
    edges: Dict[str, List[str]] = {child.name: [] for child in children}
    for offer in offers:
        if offer.source.kind == REF_CHILD and offer.target.child not in edges[offer.source.child]:
            edges[offer.source.child].append(offer.target.child)

    unvisited, on_path, done = 0, 1, 2
    state = {name: unvisited for name in edges}
    path: List[str] = []

    def visit(name: str) -> None:
        state[name] = on_path
        path.append(name)
        for successor in edges[name]:
            if state[successor] == on_path:
                cycle = path[path.index(successor) :] + [successor]
                raise ManifestError(
                    f"{where}: offers form a cycle: " + " -> ".join(f"#{n}" for n in cycle)
                )
            if state[successor] == unvisited:
                visit(successor)
        path.pop()
        state[name] = done

    for child in children:
        if state[child.name] == unvisited:
            visit(child.name)


def compile_manifest(
    source: Dict[str, Any],
    program_names: List[Dict[str, Any]],
    children_meta: List[Dict[str, Any]],
) -> Decl:
    """Checks one manifest and returns what it declares.

    |source| is the target's scope as GN wrote it: `label`, and `program`,
    `children` and `manifest` when the target has them. |program_names| is
    the `user_program_names` metadata of the program's install name target
    (empty without a program), |children_meta| the `component_manifest`
    metadata of the children. Raises ManifestError on the first rule broken.
    """
    where = source.get("label", "<manifest>")
    _check_keys(source, ("label", "program", "children", "manifest"), where)
    manifest = _check_keys(
        source.get("manifest", {}),
        ("program", "capabilities", "use", "offer", "expose"),
        f"{where}.manifest",
    )

    name = where.rsplit(":", 1)[-1]
    if not name.endswith(".cm"):
        raise ManifestError(f"{where}: a manifest target's name ends in .cm")
    check_name(name[: -len(".cm")], f"{where}: the manifest's name")

    decl = Decl()
    decl.program = _program(source, program_names, where)
    decl.children = _children(source, children_meta, where)
    children = {child.name: index for index, child in enumerate(decl.children)}
    decl.capabilities = _capabilities(manifest, f"{where}.manifest")
    declared = {capability.name: capability.kind for capability in decl.capabilities}
    decl.uses = _uses(manifest, f"{where}.manifest")
    decl.offers = _offers(manifest, children, declared, f"{where}.manifest")
    decl.exposes = _exposes(manifest, children, declared, f"{where}.manifest")
    _check_cycles(decl.offers, decl.children, where)
    return decl


class _Strings:
    """The string table: every string once, in the order first asked for."""

    def __init__(self) -> None:
        self._data = bytearray()
        self._offsets: Dict[str, int] = {}

    def add(self, text: str) -> Tuple[int, int]:
        encoded = text.encode()
        if text not in self._offsets:
            self._offsets[text] = len(self._data)
            self._data += encoded
        return self._offsets[text], len(encoded)

    def data(self) -> bytes:
        return bytes(self._data)


def encode(decl: Decl) -> bytes:
    """|decl| in the compiled format: the header, the record tables in the
    order the header counts them, then the string table."""
    strings = _Strings()
    children = {child.name: index for index, child in enumerate(decl.children)}

    def ref(value: Ref) -> Tuple[int, int]:
        if value.kind == REF_CHILD:
            return value.kind, children[value.child]
        return value.kind, 0

    def rights(value: Optional[int]) -> Tuple[int, int]:
        if value is None:
            return 0, 0
        return FLAG_RIGHTS_STATED, value

    tables = bytearray()
    programs = [decl.program] if decl.program is not None else []
    args = decl.program.args if decl.program is not None else []
    for program in programs:
        tables += PROGRAM.pack(RUNNER_ELF, 0, *strings.add(program.binary))
    for arg in args:
        tables += ARG.pack(*strings.add(arg))
    for child in decl.children:
        tables += CHILD.pack(*strings.add(child.name), *strings.add(child.url), STARTUP_EAGER, 0)
    for capability in decl.capabilities:
        tables += CAPABILITY.pack(capability.kind, 0, *strings.add(capability.name))
    for use in decl.uses:
        flags, mask = rights(use.rights)
        tables += USE.pack(
            use.kind,
            flags,
            REF_PARENT,
            0,
            *strings.add(use.source_name),
            *strings.add(use.target_path),
            *strings.add(use.subdir),
            mask,
        )
    for offer in decl.offers:
        flags, mask = rights(offer.rights)
        tables += OFFER.pack(
            offer.kind,
            flags,
            *ref(offer.source),
            *strings.add(offer.source_name),
            *ref(offer.target),
            *strings.add(offer.target_name),
            *strings.add(offer.subdir),
            mask,
        )
    for expose in decl.exposes:
        tables += EXPOSE.pack(
            expose.kind,
            0,
            *ref(expose.source),
            *strings.add(expose.source_name),
            REF_PARENT,
            0,
            *strings.add(expose.target_name),
        )

    table = strings.data()
    total = HEADER.size + len(tables) + len(table)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        0,
        total,
        len(table),
        len(programs),
        len(args),
        len(decl.children),
        len(decl.capabilities),
        len(decl.uses),
        len(decl.offers),
        len(decl.exposes),
        0,
    )
    return header + bytes(tables) + table


def _load(path: str) -> Any:
    with open(path, encoding="utf-8") as file:
        return json.load(file)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True, help="the target's scope, as JSON")
    parser.add_argument("--program", required=True, help="the program's install name, as JSON")
    parser.add_argument("--children", required=True, help="the children's meta paths, as JSON")
    parser.add_argument("--output", required=True, help="the .cm file to write")
    args = parser.parse_args(argv)

    try:
        decl = compile_manifest(_load(args.source), _load(args.program), _load(args.children))
    except ManifestError as error:
        print(f"component_manifest: {error}", file=sys.stderr)
        return 1

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(encode(decl))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
