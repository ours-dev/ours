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

A manifest with children also checks its whole tree (--tree lists every
descendant's scope): each protocol use is walked the way route() walks it, up
through the parents' offers and down through the children's exposes, and a
route that ends at a capability of another protocol type is an error. A route
that does not resolve is left to run time. "The type check" below has the
rules.

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

# 2 since a protocol capability and a protocol use carry a type string. The
# decoder refuses any other version, and no manifest outlives its build.
VERSION = 2

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
CAPABILITY = struct.Struct("<IIIIII")  # kind, reserved, name, type: 24 bytes
USE = struct.Struct("<IIIIIIIIIIIIQ")  # 56 bytes, the rights at offset 48
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
    # The protocol it speaks, as the manifest wrote it. Empty when the
    # manifest states none, which the decoder reads as the name.
    type: str = ""


@dataclass
class Use:
    kind: int
    source_name: str
    target_path: str
    subdir: str
    rights: Optional[int]
    # As Capability.type, for a protocol use. Always empty for a directory.
    type: str = ""


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


def _stated_type(clause: Dict[str, Any], where: str) -> str:
    """The `type` a protocol capability or use states, or "" when it states
    none.

    A type names the protocol, so it goes with one protocol: a list would
    give every name in it the same type, which is not what a list of names
    means anywhere else. It is spelled like a name, being the protocol's own.
    """
    if "type" not in clause:
        return ""
    if isinstance(clause["protocol"], list):
        raise ManifestError(
            f"{where}.type: `type` goes with a single protocol, not a list; write a clause "
            "for each protocol that needs one"
        )
    return check_name(clause["type"], f"{where}.type")


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
        clause = _check_keys(clause, ("protocol", "type"), at)
        if "protocol" not in clause:
            raise ManifestError(f"{at}: needs `protocol`")
        names = _one_or_many_names(clause["protocol"], f"{at}.protocol")
        type_ = _stated_type(clause, at)
        for name in names:
            if name in seen:
                raise ManifestError(f"{at}: '{name}' is a duplicate capability name")
            seen.add(name)
            capabilities.append(Capability(KIND_PROTOCOL, name, type_))
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
            clause, ("protocol", "directory", "from", "path", "rights", "subdir", "type"), at
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
            type_ = _stated_type(clause, at)
            for name in names:
                uses.append(Use(kind, name, f"/svc/{name}", "", None, type_))
                paths.append((f"/svc/{name}", f"{at} protocol '{name}'"))
            continue

        if "type" in clause:
            raise ManifestError(
                f"{at}.type: a directory use has no `type`; only a protocol has one, and a "
                "directory's hops are compared by their rights"
            )
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
        if isinstance(clause, dict) and "type" in clause:
            raise ManifestError(
                f"{at}.type: an offer has no `type`: `as` renames the capability, not the "
                "protocol. State the type where the capability is declared (`capabilities`) "
                "and where it is used (`use`)"
            )
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
        if isinstance(clause, dict) and "type" in clause:
            raise ManifestError(
                f"{at}.type: an expose has no `type`: `as` renames the capability, not the "
                "protocol. State the type where the capability is declared (`capabilities`) "
                "and where it is used (`use`)"
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


def _check_source(source: Dict[str, Any]) -> Tuple[str, Dict[str, Any]]:
    """The rules a target's scope must keep before anything in it is read:
    only the keys GN writes, only the manifest keys the language has, and a
    name that ends in .cm. Returns the target's label and its `manifest`."""
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
    return where, manifest


def _declare(decl: Decl, manifest: Dict[str, Any], where: str) -> None:
    """Fills in what |manifest| declares and offers, once |decl| holds its
    children: its capabilities, uses, offers and exposes."""
    children = {child.name: index for index, child in enumerate(decl.children)}
    decl.capabilities = _capabilities(manifest, f"{where}.manifest")
    declared = {capability.name: capability.kind for capability in decl.capabilities}
    decl.uses = _uses(manifest, f"{where}.manifest")
    decl.offers = _offers(manifest, children, declared, f"{where}.manifest")
    decl.exposes = _exposes(manifest, children, declared, f"{where}.manifest")
    _check_cycles(decl.offers, decl.children, where)


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
    where, manifest = _check_source(source)
    decl = Decl()
    decl.program = _program(source, program_names, where)
    decl.children = _children(source, children_meta, where)
    _declare(decl, manifest, where)
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
        tables += CAPABILITY.pack(
            capability.kind, 0, *strings.add(capability.name), *strings.add(capability.type)
        )
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
            *strings.add(use.type),
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


# --- The type check --------------------------------------------------------
#
# A route that ends at a capability of another protocol type is stopped twice:
# here, when the image is built, and by route() in the root on the first Open
# (docs/design/261029/protocol-identity.md, "Interception"). The walk below
# is route()'s, written out again in Python for a protocol use, since the
# root's is C++ and this runs in the build. Two copies of one walk can drift,
# so each rule keeps the name route()'s spec gives it (the comment on route()
# in //system/lib/component/include/component/route.hpp, "The walk"), the
# lookups are named for the detail:: ones in src/route.cpp, and
# test_component_manifest.py walks the fixtures route_test.cpp walks and
# compares the two sets of answers.
#
# It differs from route() in four ways, all on purpose:
# - A protocol use, offer and expose state no rights and no subdir, so the hop
#   rules (detail::Hops) have nothing to apply. A directory use is not walked
#   at all: its hops are compared by their rights, not by a type.
# - A route that does not resolve is no error here. route() would answer
#   NotFound or AccessDenied for it; the build leaves that to run time, as
#   before, so that routing_checks' deliberate failures still build.
# - "The root" is the top of the tree being compiled. Above it are the
#   built-ins when a list of them was given, and nothing when none was, where
#   the walk stops unresolved.
# - Every child is already resolved: the tree holds the declaration of each
#   descendant, read from --tree, so route()'s "a child that cannot be
#   resolved" has no counterpart. A child missing from the tree is a broken
#   build, not a route that leads nowhere.


@dataclass
class Instance:
    """One component instance of the tree being checked.

    A manifest that is a child of two parents is two instances: the walk goes
    through the parent, so the same manifest can route differently under each.
    """

    label: str
    # The child names from the root down, "" for the root: `("pc-ps2",)`, or
    # `("a", "b")` for #a/#b.
    path: Tuple[str, ...]
    decl: Decl
    has_program: bool
    parent: Optional["Instance"] = None
    children: Dict[str, "Instance"] = field(default_factory=dict)


@dataclass
class End:
    """Where a route that resolved ended: at the capability |name| that
    |instance| declares, or at the built-in |name| when |instance| is None."""

    instance: Optional[Instance]
    name: str
    # The protocol it speaks, never empty: the declared type, or its name.
    type: str


@dataclass
class Resolved:
    """A route that resolved: each hop from the use to the end, as the line
    the build error prints, and the end."""

    hops: List[str]
    end: End


@dataclass
class Mismatch:
    """A use whose route ended at a capability of another type."""

    # The manifest being compiled: its label, and its name as the root's hops
    # spell it.
    label: str
    root_name: str
    instance: Instance
    use: Use
    want: str
    resolved: Resolved


def _where(instance: Instance, root_name: str) -> str:
    """How an error names |instance|: its path from the root, `#a/#b`, or the
    root manifest's own name for the root."""
    if not instance.path:
        return root_name
    return "/".join(f"#{name}" for name in instance.path)


def _find_builtin(builtins: Optional[List[Any]], kind: int, name: str) -> Optional[Any]:
    """The first built-in of |kind| named |name|, as detail::find_builtin().
    None when no list was given."""
    for builtin in builtins or []:
        if builtin.kind == kind and builtin.name == name:
            return builtin
    return None


def _find_offer(decl: Decl, kind: int, child: str, name: str) -> Optional[Offer]:
    """The first of |decl|'s offers of |kind| to its child |child| whose
    target_name is |name|, as detail::find_offer()."""
    for offer in decl.offers:
        if offer.kind == kind and offer.target.child == child and offer.target_name == name:
            return offer
    return None


def _find_expose(decl: Decl, kind: int, name: str) -> Optional[Expose]:
    """The first of |decl|'s exposes of |kind| to its parent whose target_name
    is |name|, as detail::find_expose(). Every expose goes to the parent."""
    for expose in decl.exposes:
        if expose.kind == kind and expose.target_name == name:
            return expose
    return None


def _find_capability(decl: Decl, kind: int, name: str) -> Optional[Capability]:
    """The first of |decl|'s own capabilities of |kind| named |name|, as
    detail::find_capability()."""
    for capability in decl.capabilities:
        if capability.kind == kind and capability.name == name:
            return capability
    return None


def _ref_text(ref: Ref) -> str:
    """A reference as a manifest writes it."""
    if ref.kind == REF_PARENT:
        return "parent"
    if ref.kind == REF_SELF:
        return "self"
    return f"#{ref.child}"


# One line of the route in an error: the word, padded to the width of the
# longest, then what the manifest says.
_HOP_WORD_WIDTH = len("expose") + 1


def _use_line(instance: Instance, use: Use, root_name: str) -> str:
    return f'{"use":<{_HOP_WORD_WIDTH}}{_where(instance, root_name)} "{use.source_name}"'


def _offer_line(parent: Instance, offer: Offer, root_name: str) -> str:
    line = (
        f'{"offer":<{_HOP_WORD_WIDTH}}{_where(parent, root_name)} "{offer.source_name}" '
        f"from {_ref_text(offer.source)} to {_ref_text(offer.target)}"
    )
    if offer.target_name != offer.source_name:
        line += f' as "{offer.target_name}"'
    return line


def _expose_line(child: Instance, expose: Expose, root_name: str) -> str:
    line = (
        f'{"expose":<{_HOP_WORD_WIDTH}}{_where(child, root_name)} "{expose.source_name}" '
        f"from {_ref_text(expose.source)} to parent"
    )
    if expose.target_name != expose.source_name:
        line += f' as "{expose.target_name}"'
    return line


def route_protocol_use(
    target: Instance, use: Use, builtins: Optional[List[Any]], root_name: str
) -> Optional[Resolved]:
    """Walks |use|, a protocol use of |target|, to the capability's source,
    as route() does; None when the route does not resolve.

    |builtins| are the entries of the root's data file (component_builtins),
    or None when none was given. The state is route()'s: an instance |at|,
    where the walk has reached, a Ref |source|, where the capability comes
    from as seen from |at|, and a |name|, the capability's name as |at| knows
    it.
    """
    kind = use.kind
    hops = [_use_line(target, use, root_name)]
    at = target
    # Every use the compiler writes comes from the parent (D3).
    source = Ref(REF_PARENT)
    name = use.source_name
    # Up through the parents' offers while the capability comes from a parent,
    # then down through the children's exposes while it comes from a child,
    # until it comes from self or from a built-in. An expose from the parent
    # is refused below, so the walk never turns back up.
    while source.kind != REF_SELF:
        if source.kind == REF_PARENT:
            parent = at.parent
            if parent is None:
                # **from is the parent, and |at| is the root**: the built-ins,
                # the walk's last hop. None: no list was given, or it names no
                # built-in of this kind and name, and route() answers NotFound.
                builtin = _find_builtin(builtins, kind, name)
                if builtin is None:
                    return None
                return Resolved(hops, End(None, builtin.name, builtin.type or builtin.name))

            # **from is the parent**: in the parent's offers, the one whose
            # target is this child and whose target_name is |name|.
            offer = _find_offer(parent.decl, kind, at.path[-1], name)
            if offer is None:
                return None
            hops.append(_offer_line(parent, offer, root_name))
            at = parent
            source = offer.source
            name = offer.source_name
            continue

        # **from is the child c**: in the child's exposes, the one whose
        # target_name is |name|. None, or one whose source is the parent (the
        # compiler never writes it, and following it would turn the walk back
        # up): the route leads nowhere.
        child = at.children[source.child]
        expose = _find_expose(child.decl, kind, name)
        if expose is None or expose.source.kind == REF_PARENT:
            return None
        hops.append(_expose_line(child, expose, root_name))
        at = child
        source = expose.source
        name = expose.source_name

    # **from is self**: |at| serves it, if it has a program to serve anything
    # and declares a capability of this kind and name.
    if not at.has_program:
        return None
    capability = _find_capability(at.decl, kind, name)
    if capability is None:
        return None
    return Resolved(hops, End(at, capability.name, capability.type or capability.name))


def _child_labels(source: Dict[str, Any], where: str) -> Dict[str, str]:
    """The label of each child by name, in the order |source| lists them."""
    labels: Dict[str, str] = {}
    for index, clause in enumerate(_list(source, "children", where)):
        at = f"{where}.children[{index}]"
        clause = _check_keys(clause, ("name", "manifest", "startup"), at)
        name = check_name(clause.get("name"), f"{at}.name")
        labels[name] = _plain_label(clause.get("manifest"), f"{at}.manifest")
    return labels


def _descendant(source: Dict[str, Any]) -> Tuple[Decl, Dict[str, str]]:
    """What the walk needs of a manifest below the one being compiled: its
    declarations, checked by the rules every manifest keeps, and the label of
    each child by name. Its own compile checked these already; reading them
    through the same rules means a file the walk cannot trust fails here, with
    the descendant's label in the message, rather than as a KeyError."""
    where, manifest = _check_source(source)
    labels = _child_labels(source, where)
    decl = Decl(children=[Child(name, "") for name in labels])
    _declare(decl, manifest, where)
    return decl, labels


def build_tree(
    source: Dict[str, Any], decl: Decl, sources: Dict[str, Dict[str, Any]]
) -> Instance:
    """The instance tree of the manifest being compiled, |source| and |decl|,
    with each descendant read from |sources| by its label. Every child is an
    instance of its own, and the same manifest under two parents is two."""
    label = source["label"]
    root = Instance(label, (), decl, decl.program is not None)

    def grow(instance: Instance, labels: Dict[str, str], chain: Tuple[str, ...]) -> None:
        for name, child_label in labels.items():
            if child_label in chain:
                raise ManifestError(
                    f"{instance.label}: #{name} is {child_label}, which is its own ancestor"
                )
            if child_label not in sources:
                raise ManifestError(
                    f"{instance.label}: #{name} is {child_label}, which is not in the tree "
                    "the build collected"
                )
            child_source = sources[child_label]
            child_decl, child_labels = _descendant(child_source)
            child = Instance(
                child_label,
                instance.path + (name,),
                child_decl,
                "program" in child_source,
                instance,
            )
            instance.children[name] = child
            grow(child, child_labels, chain + (child_label,))

    grow(root, _child_labels(source, label), (label,))
    return root


def instances(root: Instance) -> List[Instance]:
    """Every instance of the tree, ordered by path."""
    found: List[Instance] = []

    def visit(instance: Instance) -> None:
        found.append(instance)
        for child in instance.children.values():
            visit(child)

    visit(root)
    return sorted(found, key=lambda instance: instance.path)


def check_types(
    source: Dict[str, Any],
    decl: Decl,
    sources: Dict[str, Dict[str, Any]],
    builtins: Optional[List[Any]],
) -> List[Mismatch]:
    """Every protocol use in the tree of the manifest being compiled whose
    route resolves and ends at a capability of another type than the use's,
    ordered by instance path and then by use.

    |source| and |decl| are the manifest being compiled, |sources| the scope
    of each descendant by its label, |builtins| the entries of the root's data
    file or None.
    """
    root = build_tree(source, decl, sources)
    label = source["label"]
    root_name = label.rsplit(":", 1)[-1]
    mismatches = []
    for instance in instances(root):
        for use in instance.decl.uses:
            if use.kind != KIND_PROTOCOL:
                continue
            resolved = route_protocol_use(instance, use, builtins, root_name)
            if resolved is None:
                continue
            want = use.type or use.source_name
            if resolved.end.type != want:
                mismatches.append(Mismatch(label, root_name, instance, use, want, resolved))
    return mismatches


def format_mismatch(mismatch: Mismatch) -> str:
    """The build error for |mismatch|: the use and what it wanted, where the
    route ended, each hop, and the two ways to fix it."""
    indent = " " * len("error: ")
    use, resolved = mismatch.use, mismatch.resolved
    end = resolved.end
    if end.instance is None:
        ends_at = "the root's built-in"
    else:
        ends_at = f"{_where(end.instance, mismatch.root_name)}'s capability"
    lines = [
        f"error: {mismatch.label}: {_where(mismatch.instance, mismatch.root_name)} uses protocol "
        f'"{use.source_name}" (type {mismatch.want}),',
        f'{indent}but the route ends at {ends_at} "{end.name}" (type {end.type}):',
    ]
    lines += [f"{indent}  {hop}" for hop in resolved.hops]
    lines.append(
        f'{indent}Offer a capability of type "{mismatch.want}" under that name, or give the '
        "use the type it means."
    )
    return "\n".join(lines) + "\n"


def load_tree(entries: Any) -> Tuple[Dict[str, Dict[str, Any]], List[str]]:
    """The scope of every descendant, by label, from the `--tree` file's
    entries `{label, source}`, and the paths that were read, for the depfile."""
    if not isinstance(entries, list):
        raise ManifestError("tree: expected a list of {label, source}")
    sources: Dict[str, Dict[str, Any]] = {}
    paths: List[str] = []
    for index, entry in enumerate(entries):
        at = f"tree[{index}]"
        entry = _check_keys(entry, ("label", "source"), at)
        label = _plain_label(entry.get("label"), f"{at}.label")
        path = _string(entry.get("source"), f"{at}.source")
        if label in sources:
            raise ManifestError(f"{at}: {label} is in the tree twice")
        sources[label] = _load(path)
        paths.append(path)
    return sources, paths


def _write_depfile(path: str, output: str, inputs: List[str]) -> None:
    """A depfile naming |inputs| as what |output| was made from. GN cannot
    list a descendant's declaration as an input, since its name is known only
    from the metadata this reads, so the build learns it from here."""
    names = " ".join(name.replace(" ", "\\ ") for name in inputs)
    pathlib.Path(path).write_text(f"{output.replace(' ', chr(92) + ' ')}: {names}\n")


def _load(path: str) -> Any:
    with open(path, encoding="utf-8") as file:
        return json.load(file)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True, help="the target's scope, as JSON")
    parser.add_argument("--program", required=True, help="the program's install name, as JSON")
    parser.add_argument("--children", required=True, help="the children's meta paths, as JSON")
    parser.add_argument(
        "--tree",
        required=True,
        help="every descendant's label and the path of its scope, as JSON: what the type "
        "check walks",
    )
    parser.add_argument(
        "--builtins",
        help="the root's built-in capabilities (component_builtins.py's data file): without "
        "it a use that routes above the top of the tree is not checked, since nothing says "
        "what it reaches",
    )
    parser.add_argument("--output", required=True, help="the .cm file to write")
    parser.add_argument(
        "--depfile",
        help="where to write the list of every scope the type check read, so that a change "
        "in any descendant's declaration reruns this compile",
    )
    parser.add_argument(
        "--no-type-check",
        action="store_true",
        help="do not check that every route in the tree ends at a capability of the type its "
        "use asks for; for a fixture that exercises the run-time check on a mismatch",
    )
    args = parser.parse_args(argv)

    try:
        scope = _load(args.source)
        decl = compile_manifest(scope, _load(args.program), _load(args.children))
        read: List[str] = []
        if not args.no_type_check:
            sources, read = load_tree(_load(args.tree))
            builtins = None
            if args.builtins is not None:
                # Not at the top: component_builtins reads this module's
                # rights words and name rules, so it imports this one.
                import component_builtins

                builtins = component_builtins.load(args.builtins)
            mismatches = check_types(scope, decl, sources, builtins)
            if mismatches:
                # The mismatches' own text starts `error:`, which ninja shows
                # as it is; the prefix every other message here carries would
                # bury it.
                print("\n".join(format_mismatch(m) for m in mismatches), end="", file=sys.stderr)
                return 1
    except ManifestError as error:
        print(f"component_manifest: {error}", file=sys.stderr)
        return 1

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(encode(decl))
    if args.depfile is not None:
        _write_depfile(args.depfile, args.output, read)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
