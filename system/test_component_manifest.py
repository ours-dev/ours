#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for component_manifest.py: each rule it checks, and what it writes.

Run from anywhere: python3 system/test_component_manifest.py

The bytes it writes are also decoded by the C++ decoder's own test
(//system/lib/component/test), from manifests the GN template built, so the
two halves of the format cannot drift apart unnoticed.

The type check walks routes the way route() does, in a second implementation.
FixtureRoutesTest compares the two on the fixtures route_test.cpp walks; it
reads the scopes GN wrote for them, so it needs `gn gen` to have run in some
build directory under out/ (or OURS_BUILD_DIR), and says so when it skips.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import struct
import sys
import tempfile
import unittest
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import component_builtins as cb  # noqa: E402
import component_manifest as cm  # noqa: E402

LABEL = "//test:test.cm"
PROGRAM = "//test:app"
PROGRAM_NAMES = [{"name": "app", "path": "bin/app", "label": PROGRAM}]


def child(name: str, startup: str = "eager") -> Dict[str, Any]:
    return {"name": name, "manifest": f"//test:{name}.cm", "startup": startup}


def child_meta(*names: str) -> List[Dict[str, str]]:
    return [{"label": f"//test:{name}.cm", "path": f"meta/{name}.cm"} for name in names]


def source(manifest: Dict[str, Any] = None, **extra: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"label": LABEL}
    if manifest is not None:
        result["manifest"] = manifest
    result.update(extra)
    return result


def compile_with_children(manifest: Dict[str, Any], *names: str) -> cm.Decl:
    return cm.compile_manifest(
        source(manifest, children=[child(name) for name in names]), [], child_meta(*names)
    )


class Reader:
    """A second, independent reading of the compiled format, so that a test
    can say what the bytes hold rather than how the encoder built them."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        fields = cm.HEADER.unpack_from(data, 0)
        (
            self.magic,
            self.version,
            _,
            self.total_size,
            self.string_table_size,
            *counts,
            _,
        ) = fields
        names = ["program", "arg", "child", "capability", "use", "offer", "expose"]
        layouts = [cm.PROGRAM, cm.ARG, cm.CHILD, cm.CAPABILITY, cm.USE, cm.OFFER, cm.EXPOSE]
        self.tables: Dict[str, List[tuple]] = {}
        offset = cm.HEADER.size
        for name, layout, count in zip(names, layouts, counts):
            rows = []
            for _ in range(count):
                rows.append(layout.unpack_from(data, offset))
                offset += layout.size
            self.tables[name] = rows
        self.strings = data[offset:]

    def text(self, offset: int, size: int) -> str:
        return self.strings[offset : offset + size].decode()


class CompileTest(unittest.TestCase):
    def refuses(self, pattern: str, *args: Any) -> None:
        with self.assertRaisesRegex(cm.ManifestError, pattern):
            cm.compile_manifest(*args)

    def test_empty_manifest(self) -> None:
        decl = cm.compile_manifest(source(), [], [])
        self.assertIsNone(decl.program)
        self.assertEqual(decl.children, [])

    def test_program_binary_and_args(self) -> None:
        decl = cm.compile_manifest(
            source({"program": {"args": ["a", "b"]}}, program=PROGRAM), PROGRAM_NAMES, []
        )
        self.assertEqual(decl.program, cm.Program("bin/app", ["a", "b"]))

    def test_program_must_live_under_bin(self) -> None:
        names = [{"name": "app", "path": "lib/app", "label": PROGRAM}]
        self.refuses("not under 'bin/'", source(program=PROGRAM), names, [])

    def test_program_args_without_a_program(self) -> None:
        self.refuses("there is no program", source({"program": {"args": []}}), [], [])

    def test_program_runner_and_binary_come_from_the_label(self) -> None:
        manifest = {"program": {"binary": "bin/app"}}
        self.refuses("unknown key 'binary'", source(manifest, program=PROGRAM), PROGRAM_NAMES, [])

    def test_child_url_follows_d26(self) -> None:
        decl = compile_with_children({}, "shell")
        self.assertEqual(decl.children, [cm.Child("shell", "fuchsia-boot:///#meta/shell.cm")])

    def test_child_names_are_unique(self) -> None:
        manifest = source(children=[child("a"), child("a")])
        self.refuses("duplicate child name", manifest, [], child_meta("a"))

    def test_lazy_is_refused(self) -> None:
        self.refuses("'lazy' is not supported", source(children=[child("a", "lazy")]), [], child_meta("a"))

    def test_startup_must_be_written(self) -> None:
        scope = {"name": "a", "manifest": "//test:a.cm"}
        self.refuses("written out as \"eager\"", source(children=[scope]), [], child_meta("a"))

    def test_a_label_that_names_a_toolchain_is_refused(self) -> None:
        scope = dict(child("a"), manifest="//test:a.cm(//build/toolchain/ours:user-x64)")
        self.refuses("names a toolchain", source(children=[scope]), [], child_meta("a"))
        program = "//test:app(//build/toolchain/ours:user-x64)"
        self.refuses("names a toolchain", source(program=program), PROGRAM_NAMES, [])

    def test_name_length(self) -> None:
        longest = "p" * cm.MAX_NAME_LENGTH
        decl = cm.compile_manifest(source({"capabilities": [{"protocol": longest}]}), [], [])
        self.assertEqual(decl.capabilities[0].name, longest)
        self.refuses(
            "longer than 100 bytes",
            source({"capabilities": [{"protocol": longest + "p"}]}),
            [],
            [],
        )

    def test_name_characters(self) -> None:
        self.refuses("is not a name", source({"capabilities": [{"protocol": ".hidden"}]}), [], [])
        self.refuses("is not a name", source({"capabilities": [{"protocol": "a/b"}]}), [], [])

    def test_capability_names_are_unique(self) -> None:
        manifest = {"capabilities": [{"protocol": "p"}, {"protocol": ["q", "p"]}]}
        self.refuses("duplicate capability name", source(manifest), [], [])

    def test_a_declared_directory_is_refused(self) -> None:
        manifest = {"capabilities": [{"directory": "data"}]}
        self.refuses("cannot declare a directory", source(manifest), [], [])

    def test_unknown_keys_are_refused(self) -> None:
        self.refuses("unknown key 'environments'", source({"environments": []}), [], [])
        manifest = {"use": [{"protocol": "p", "availability": "optional"}]}
        self.refuses("unknown key 'availability'", source(manifest), [], [])

    def test_protocol_use_is_at_svc(self) -> None:
        decl = cm.compile_manifest(source({"use": [{"protocol": ["a", "b"]}]}), [], [])
        self.assertEqual(
            decl.uses,
            [
                cm.Use(cm.KIND_PROTOCOL, "a", "/svc/a", "", None),
                cm.Use(cm.KIND_PROTOCOL, "b", "/svc/b", "", None),
            ],
        )

    def test_protocol_use_with_path_is_refused(self) -> None:
        manifest = {"use": [{"protocol": "p", "path": "/svc/q"}]}
        self.refuses("has no `path`", source(manifest), [], [])

    def test_use_from_anything_but_the_parent_is_refused(self) -> None:
        manifest = {"use": [{"protocol": "p", "from": "self"}]}
        self.refuses("from the parent only", source(manifest), [], [])

    def test_directory_use(self) -> None:
        manifest = {"use": [{"directory": "boot", "path": "/boot", "rights": ["rx*"], "subdir": "bin"}]}
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual(
            decl.uses, [cm.Use(cm.KIND_DIRECTORY, "boot", "/boot", "bin", cm.RIGHTS["rx*"])]
        )

    def test_directory_use_needs_path_and_rights(self) -> None:
        self.refuses("needs `path` and `rights`", source({"use": [{"directory": "boot", "path": "/boot"}]}), [], [])

    def test_use_paths_are_unique(self) -> None:
        manifest = {
            "use": [
                {"directory": "a", "path": "/data", "rights": ["r*"]},
                {"directory": "b", "path": "/data", "rights": ["r*"]},
            ]
        }
        self.refuses("duplicate use path", source(manifest), [], [])
        self.refuses("duplicate use path", source({"use": [{"protocol": "p"}, {"protocol": "p"}]}), [], [])

    def test_use_paths_do_not_nest(self) -> None:
        manifest = {
            "use": [
                {"directory": "a", "path": "/data", "rights": ["r*"]},
                {"directory": "b", "path": "/data/b", "rights": ["r*"]},
            ]
        }
        self.refuses("are nested", source(manifest), [], [])
        manifest = {"use": [{"protocol": "p"}, {"directory": "a", "path": "/svc", "rights": ["r*"]}]}
        self.refuses("are nested", source(manifest), [], [])

    def test_sibling_paths_with_a_common_prefix_are_not_nested(self) -> None:
        manifest = {
            "use": [
                {"directory": "a", "path": "/data", "rights": ["r*"]},
                {"directory": "b", "path": "/database", "rights": ["r*"]},
            ]
        }
        self.assertEqual(len(cm.compile_manifest(source(manifest), [], []).uses), 2)

    def test_paths(self) -> None:
        for path in ["boot", "/", "/boot/", "/boot//bin", "/boot/../x", "/./boot"]:
            manifest = {"use": [{"directory": "a", "path": path, "rights": ["r*"]}]}
            self.refuses("path", source(manifest), [], [])
        for subdir in ["/bin", "bin/", "a/../b", ""]:
            manifest = {"use": [{"directory": "a", "path": "/a", "rights": ["r*"], "subdir": subdir}]}
            self.refuses("subdir|segment", source(manifest), [], [])

    def test_rights_words(self) -> None:
        manifest = {"use": [{"directory": "a", "path": "/a", "rights": ["read_bytes", "traverse"]}]}
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual(decl.uses[0].rights, 0x02 | 0x80)
        for rights, error in [
            (["read"], "unknown right 'read'"),
            (["r*", "r*"], "duplicated in the rights clause"),
            ([], "non-empty list"),
        ]:
            manifest = {"use": [{"directory": "a", "path": "/a", "rights": rights}]}
            self.refuses(error, source(manifest), [], [])

    def test_rights_aliases_match_ours_io(self) -> None:
        # R_STAR_DIR, RX_STAR_DIR and W_STAR_DIR in <ours_io/ours_io.hpp>.
        self.assertEqual(cm.RIGHTS["r*"], 0xD3)
        self.assertEqual(cm.RIGHTS["rx*"], 0xDB)
        self.assertEqual(cm.RIGHTS["w*"], 0x1E5)

    def test_offer_from_parent_self_and_child(self) -> None:
        manifest = {
            "capabilities": [{"protocol": "mine"}],
            "offer": [
                {"directory": "boot", "from": "parent", "to": "#b", "rights": ["r*"], "subdir": "bin", "as": "bin"},
                {"protocol": "mine", "from": "self", "to": ["#a", "#b"]},
                {"protocol": "echo", "from": "#a", "to": "#b", "as": "echo2"},
            ],
        }
        decl = compile_with_children(manifest, "a", "b")
        self.assertEqual(
            decl.offers,
            [
                cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_PARENT), "boot", cm.Ref(cm.REF_CHILD, "b"), "bin", "bin", cm.RIGHTS["r*"]),
                cm.Offer(cm.KIND_PROTOCOL, cm.Ref(cm.REF_SELF), "mine", cm.Ref(cm.REF_CHILD, "a"), "mine", "", None),
                cm.Offer(cm.KIND_PROTOCOL, cm.Ref(cm.REF_SELF), "mine", cm.Ref(cm.REF_CHILD, "b"), "mine", "", None),
                cm.Offer(cm.KIND_PROTOCOL, cm.Ref(cm.REF_CHILD, "a"), "echo", cm.Ref(cm.REF_CHILD, "b"), "echo2", "", None),
            ],
        )

    def test_offer_from_self_names_a_declared_capability(self) -> None:
        manifest = {"offer": [{"protocol": "p", "from": "self", "to": "#a"}]}
        self.refuses("does not declare", source(manifest, children=[child("a")]), [], child_meta("a"))

    def test_child_references_exist(self) -> None:
        for offer in [
            {"protocol": "p", "from": "#nobody", "to": "#a"},
            {"protocol": "p", "from": "parent", "to": "#nobody"},
            {"protocol": "p", "from": "parent", "to": "a"},
        ]:
            self.refuses("names no child|not a child reference", source({"offer": [offer]}, children=[child("a")]), [], child_meta("a"))
        manifest = {"expose": [{"protocol": "p", "from": "#nobody"}]}
        self.refuses("names no child", source(manifest), [], [])

    def test_offer_target_differs_from_source(self) -> None:
        manifest = {"offer": [{"protocol": "p", "from": "#a", "to": "#a"}]}
        self.refuses("offered its own capability", source(manifest, children=[child("a")]), [], child_meta("a"))

    def test_offer_target_names_are_unique(self) -> None:
        manifest = {
            "offer": [
                {"protocol": "p", "from": "parent", "to": "#a"},
                {"protocol": "q", "from": "parent", "to": "#a", "as": "p"},
            ]
        }
        self.refuses("duplicate offer target capability", source(manifest, children=[child("a")]), [], child_meta("a"))
        # Across kinds too: the check is keyed on the name alone.
        manifest = {
            "offer": [
                {"protocol": "boot", "from": "parent", "to": "#a"},
                {"directory": "boot", "from": "parent", "to": "#a"},
            ]
        }
        self.refuses("duplicate offer target capability", source(manifest, children=[child("a")]), [], child_meta("a"))

    def test_as_renames_one_capability(self) -> None:
        manifest = {"offer": [{"protocol": ["p", "q"], "from": "parent", "to": "#a", "as": "r"}]}
        self.refuses("renames one capability", source(manifest, children=[child("a")]), [], child_meta("a"))

    def test_directory_offers_come_from_the_parent(self) -> None:
        for origin in ["self", "#b"]:
            manifest = {"offer": [{"directory": "boot", "from": origin, "to": "#a"}]}
            self.refuses("is not one of: parent", source(manifest, children=[child("a"), child("b")]), [], child_meta("a", "b"))

    def test_rights_and_subdir_are_for_directories(self) -> None:
        manifest = {"offer": [{"protocol": "p", "from": "parent", "to": "#a", "rights": ["r*"]}]}
        self.refuses("only a directory offer", source(manifest, children=[child("a")]), [], child_meta("a"))

    def test_offer_cycle_is_refused(self) -> None:
        manifest = {
            "offer": [
                {"protocol": "p", "from": "#a", "to": "#b"},
                {"protocol": "q", "from": "#b", "to": "#c"},
                {"protocol": "r", "from": "#c", "to": "#a"},
            ]
        }
        self.refuses(
            r"cycle: #a -> #b -> #c -> #a",
            source(manifest, children=[child("a"), child("b"), child("c")]),
            [],
            child_meta("a", "b", "c"),
        )

    def test_a_diamond_is_not_a_cycle(self) -> None:
        manifest = {
            "offer": [
                {"protocol": "p", "from": "#a", "to": ["#b", "#c"]},
                {"protocol": "q", "from": "#b", "to": "#d"},
                {"protocol": "r", "from": "#c", "to": "#d"},
            ]
        }
        decl = compile_with_children(manifest, "a", "b", "c", "d")
        self.assertEqual(len(decl.offers), 4)

    def test_expose(self) -> None:
        manifest = {
            "capabilities": [{"protocol": "p"}],
            "expose": [
                {"protocol": "p", "from": "self"},
                {"protocol": "q", "from": "#a", "as": "r", "to": "parent"},
            ],
        }
        decl = cm.compile_manifest(source(manifest, children=[child("a")]), [], child_meta("a"))
        self.assertEqual(
            decl.exposes,
            [
                cm.Expose(cm.KIND_PROTOCOL, cm.Ref(cm.REF_SELF), "p", "p"),
                cm.Expose(cm.KIND_PROTOCOL, cm.Ref(cm.REF_CHILD, "a"), "q", "r"),
            ],
        )

    def test_an_exposed_directory_is_refused(self) -> None:
        manifest = {"expose": [{"directory": "boot", "from": "self"}]}
        self.refuses("cannot expose a directory", source(manifest), [], [])

    def test_expose_from_self_names_a_declared_capability(self) -> None:
        self.refuses("does not declare", source({"expose": [{"protocol": "p", "from": "self"}]}), [], [])

    def test_expose_target_names_are_unique(self) -> None:
        manifest = {
            "capabilities": [{"protocol": "p"}, {"protocol": "q"}],
            "expose": [
                {"protocol": "p", "from": "self"},
                {"protocol": "q", "from": "self", "as": "p"},
            ],
        }
        self.refuses("duplicate expose target capability", source(manifest), [], [])

    def test_a_capability_and_a_use_state_a_type(self) -> None:
        manifest = {
            "capabilities": [{"protocol": "ours.example.Echo", "type": "ours.example.EchoV2"}],
            "use": [{"protocol": "echo", "type": "ours.example.Echo"}],
        }
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual(
            decl.capabilities,
            [cm.Capability(cm.KIND_PROTOCOL, "ours.example.Echo", "ours.example.EchoV2")],
        )
        self.assertEqual(
            decl.uses,
            [cm.Use(cm.KIND_PROTOCOL, "echo", "/svc/echo", "", None, "ours.example.Echo")],
        )

    def test_a_type_equal_to_the_name_is_legal(self) -> None:
        manifest = {"capabilities": [{"protocol": "p", "type": "p"}], "use": [{"protocol": "q", "type": "q"}]}
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual((decl.capabilities[0].type, decl.uses[0].type), ("p", "q"))

    def test_an_omitted_type_is_empty(self) -> None:
        manifest = {"capabilities": [{"protocol": ["p", "q"]}], "use": [{"protocol": ["r", "s"]}]}
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual([capability.type for capability in decl.capabilities], ["", ""])
        self.assertEqual([use.type for use in decl.uses], ["", ""])

    def test_a_type_goes_with_a_single_protocol(self) -> None:
        for key, clause in [
            ("capabilities", {"protocol": ["p", "q"], "type": "t"}),
            ("use", {"protocol": ["p", "q"], "type": "t"}),
            # A list of one is still a list.
            ("capabilities", {"protocol": ["p"], "type": "t"}),
            ("use", {"protocol": ["p"], "type": "t"}),
        ]:
            self.refuses(
                r"\.type: `type` goes with a single protocol, not a list",
                source({key: [clause]}),
                [],
                [],
            )

    def test_a_directory_use_has_no_type(self) -> None:
        manifest = {"use": [{"directory": "boot", "path": "/boot", "rights": ["r*"], "type": "t"}]}
        self.refuses(r"use\[0\]\.type: a directory use has no `type`", source(manifest), [], [])

    def test_an_offer_has_no_type(self) -> None:
        for offer in [
            {"protocol": "p", "from": "parent", "to": "#a", "type": "t"},
            {"protocol": "p", "from": "parent", "to": "#a", "as": "q", "type": "t"},
            {"directory": "boot", "from": "parent", "to": "#a", "type": "t"},
        ]:
            self.refuses(
                r"offer\[0\]\.type: an offer has no `type`: `as` renames the capability, not the protocol",
                source({"offer": [offer]}, children=[child("a")]),
                [],
                child_meta("a"),
            )

    def test_an_expose_has_no_type(self) -> None:
        manifest = {
            "capabilities": [{"protocol": "p"}],
            "expose": [{"protocol": "p", "from": "self", "as": "q", "type": "t"}],
        }
        self.refuses(
            r"expose\[0\]\.type: an expose has no `type`: `as` renames the capability, not the protocol",
            source(manifest),
            [],
            [],
        )

    def test_a_type_follows_the_rules_of_a_name(self) -> None:
        for key in ["capabilities", "use"]:
            for bad, error in [
                ("", "must not be empty"),
                (".hidden", "is not a name"),
                ("a/b", "is not a name"),
                ("p" * (cm.MAX_NAME_LENGTH + 1), "longer than 100 bytes"),
                (7, "expected a string"),
                ("a\0b", "NUL byte"),
            ]:
                self.refuses(
                    error, source({key: [{"protocol": "p", "type": bad}]}), [], []
                )
        longest = "t" * cm.MAX_NAME_LENGTH
        decl = cm.compile_manifest(
            source({"use": [{"protocol": "p", "type": longest}]}), [], []
        )
        self.assertEqual(decl.uses[0].type, longest)

    def test_manifest_name(self) -> None:
        self.refuses("ends in .cm", {"label": "//test:test"}, [], [])
        self.refuses("not a name", {"label": "//test:.hidden.cm"}, [], [])


class EncodeTest(unittest.TestCase):
    def compiled(self) -> bytes:
        manifest = {
            "program": {"args": ["--serve", "echo"]},
            "capabilities": [{"protocol": "ours.example.Echo"}],
            "use": [
                {"directory": "boot", "path": "/boot", "rights": ["rx*"]},
                {"protocol": "ours.process.Launcher"},
            ],
            "offer": [{"protocol": "ours.example.Echo", "from": "self", "to": "#child"}],
            "expose": [{"protocol": "ours.example.Echo", "from": "self"}],
        }
        decl = cm.compile_manifest(
            source(manifest, program=PROGRAM, children=[child("child")]),
            PROGRAM_NAMES,
            child_meta("child"),
        )
        return cm.encode(decl)

    def test_record_sizes_match_format_hpp(self) -> None:
        # The static_asserts in format.hpp say the same.
        sizes = [
            (cm.HEADER, 48),
            (cm.PROGRAM, 16),
            (cm.ARG, 8),
            (cm.CHILD, 24),
            (cm.CAPABILITY, 24),
            (cm.USE, 56),
            (cm.OFFER, 56),
            (cm.EXPOSE, 40),
        ]
        for layout, size in sizes:
            self.assertEqual(layout.size, size)

    def test_header(self) -> None:
        data = self.compiled()
        reader = Reader(data)
        self.assertEqual(data[:4], b"OSCM")
        self.assertEqual(reader.magic, cm.MAGIC)
        self.assertEqual(reader.version, cm.VERSION)
        self.assertEqual(reader.total_size, len(data))
        self.assertEqual(len(reader.strings), reader.string_table_size)
        counts = {name: len(rows) for name, rows in reader.tables.items()}
        self.assertEqual(
            counts,
            {"program": 1, "arg": 2, "child": 1, "capability": 1, "use": 2, "offer": 1, "expose": 1},
        )

    def test_records(self) -> None:
        reader = Reader(self.compiled())
        runner, reserved, *binary = reader.tables["program"][0]
        self.assertEqual((runner, reserved, reader.text(*binary)), (cm.RUNNER_ELF, 0, "bin/app"))
        self.assertEqual([reader.text(*arg) for arg in reader.tables["arg"]], ["--serve", "echo"])

        name_offset, name_size, url_offset, url_size, startup, reserved = reader.tables["child"][0]
        self.assertEqual(reader.text(name_offset, name_size), "child")
        self.assertEqual(reader.text(url_offset, url_size), "fuchsia-boot:///#meta/child.cm")
        self.assertEqual((startup, reserved), (cm.STARTUP_EAGER, 0))

        directory, protocol = reader.tables["use"]
        kind, flags, source_kind, source_child, *strings, rights = directory
        self.assertEqual((kind, flags, source_kind, source_child), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED, cm.REF_PARENT, 0))
        texts = [reader.text(strings[i], strings[i + 1]) for i in range(0, 8, 2)]
        self.assertEqual(texts, ["boot", "/boot", "", ""])
        self.assertEqual(rights, cm.RIGHTS["rx*"])
        kind, flags, *_, rights = protocol
        self.assertEqual((kind, flags, rights), (cm.KIND_PROTOCOL, 0, 0))

        offer = reader.tables["offer"][0]
        self.assertEqual(offer[2:4], (cm.REF_SELF, 0))
        self.assertEqual(offer[6:8], (cm.REF_CHILD, 0))

        expose = reader.tables["expose"][0]
        self.assertEqual(expose[2:4], (cm.REF_SELF, 0))
        self.assertEqual(expose[6:8], (cm.REF_PARENT, 0))

    def test_the_format_is_version_two(self) -> None:
        self.assertEqual(cm.VERSION, 2)
        self.assertEqual(Reader(self.compiled()).version, 2)

    def test_the_type_strings_sit_where_format_hpp_puts_them(self) -> None:
        # offsetof(Capability, type) == 16, offsetof(Use, type) == 40 and
        # offsetof(Use, rights) == 48 in format.hpp.
        capability = cm.CAPABILITY.pack(*range(1, 7))
        self.assertEqual(struct.unpack_from("<II", capability, 16), (5, 6))
        use = cm.USE.pack(*range(1, 13), 99)
        self.assertEqual(struct.unpack_from("<II", use, 40), (11, 12))
        self.assertEqual(struct.unpack_from("<Q", use, 48), (99,))

    def compiled_with_types(self, capability: Dict[str, Any], use: Dict[str, Any]) -> Reader:
        manifest = {"capabilities": [capability], "use": [use]}
        return Reader(cm.encode(cm.compile_manifest(source(manifest), [], [])))

    def test_an_omitted_type_writes_an_empty_string(self) -> None:
        reader = self.compiled_with_types({"protocol": "p"}, {"protocol": "q"})
        (capability,) = reader.tables["capability"]
        self.assertEqual(capability[4:6][1], 0)
        self.assertEqual(reader.text(*capability[4:6]), "")
        (use,) = reader.tables["use"]
        self.assertEqual(use[10:12][1], 0)
        self.assertEqual(reader.text(*use[10:12]), "")
        # No new bytes in the string table: the two names and the use's path.
        self.assertEqual(reader.strings, b"pq/svc/q")

    def test_an_explicit_type_writes_its_string(self) -> None:
        reader = self.compiled_with_types(
            {"protocol": "p", "type": "t.Cap"}, {"protocol": "q", "type": "t.Use"}
        )
        (capability,) = reader.tables["capability"]
        self.assertEqual(reader.text(*capability[2:4]), "p")
        self.assertEqual(reader.text(*capability[4:6]), "t.Cap")
        (use,) = reader.tables["use"]
        self.assertEqual(reader.text(*use[4:6]), "q")
        self.assertEqual(reader.text(*use[6:8]), "/svc/q")
        self.assertEqual(reader.text(*use[10:12]), "t.Use")
        self.assertEqual(use[12], 0)

    def test_an_explicit_type_equal_to_the_name_shares_its_string(self) -> None:
        # The string table stores each string once, so a type that spells the
        # name adds no byte and points at the name's own.
        omitted = self.compiled_with_types({"protocol": "p"}, {"protocol": "q"})
        stated = self.compiled_with_types({"protocol": "p", "type": "p"}, {"protocol": "q", "type": "q"})
        self.assertEqual(stated.strings, omitted.strings)
        (capability,) = stated.tables["capability"]
        self.assertEqual(capability[2:4], capability[4:6])
        (use,) = stated.tables["use"]
        self.assertEqual(use[4:6], use[10:12])

    def test_a_directory_use_writes_no_type(self) -> None:
        manifest = {"use": [{"directory": "boot", "path": "/boot", "rights": ["rx*"]}]}
        reader = Reader(cm.encode(cm.compile_manifest(source(manifest), [], [])))
        (use,) = reader.tables["use"]
        self.assertEqual(use[10:12][1], 0)
        self.assertEqual(use[12], cm.RIGHTS["rx*"])

    def test_strings_are_stored_once(self) -> None:
        reader = Reader(self.compiled())
        self.assertEqual(reader.strings.count(b"ours.example.Echo"), 1)

    def test_every_u64_is_aligned(self) -> None:
        # The rights words fall on multiples of 8 in each record, and every
        # record size is one, so a reader that maps the file sees them
        # naturally aligned.
        self.assertEqual(struct.calcsize("<IIIIIIIIII"), 40)
        self.assertEqual(struct.calcsize("<IIIIIIIIIIII"), 48)
        for layout in [cm.HEADER, cm.PROGRAM, cm.ARG, cm.CHILD, cm.CAPABILITY, cm.USE, cm.OFFER, cm.EXPOSE]:
            self.assertEqual(layout.size % 8, 0)


class MainTest(unittest.TestCase):
    def run_main(
        self,
        scope: Dict[str, Any],
        names: List[Any],
        children: List[Any],
        extra: tuple = (),
        tree: List[Any] = (),
    ) -> tuple:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = {}
            for key, value in [
                ("source", scope),
                ("program", names),
                ("children", children),
                ("tree", list(tree)),
            ]:
                paths[key] = root / f"{key}.json"
                paths[key].write_text(json.dumps(value))
            output = root / "out" / "test.cm"
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                status = cm.main(
                    [
                        "--source", str(paths["source"]),
                        "--program", str(paths["program"]),
                        "--children", str(paths["children"]),
                        "--tree", str(paths["tree"]),
                        "--output", str(output),
                        *extra,
                    ]
                )
            data = output.read_bytes() if output.exists() else None
        return status, data, errors.getvalue()

    def test_writes_the_manifest(self) -> None:
        status, data, errors = self.run_main(source(), [], [])
        self.assertEqual((status, errors), (0, ""))
        self.assertEqual(Reader(data).total_size, len(data))

    def test_a_tree_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ["source", "program", "children"]:
                (root / f"{name}.json").write_text("[]" if name != "source" else "{}")
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                cm.main(
                    [
                        "--source", str(root / "source.json"),
                        "--program", str(root / "program.json"),
                        "--children", str(root / "children.json"),
                        "--output", str(root / "test.cm"),
                    ]
                )

    def test_refusal_writes_nothing(self) -> None:
        status, data, errors = self.run_main(source({"bogus": 1}), [], [])
        self.assertEqual(status, 1)
        self.assertIsNone(data)
        self.assertIn("unknown key 'bogus'", errors)


# --- The type check ----------------------------------------------------------


def scope_of(
    name: str,
    *,
    program: bool = False,
    children: tuple = (),
    **manifest: Any,
) -> Dict[str, Any]:
    """The scope GN writes for //test:<name>.cm. A child is a name, whose
    manifest is //test:<name>.cm, or a (name, manifest name) pair."""
    result: Dict[str, Any] = {"label": f"//test:{name}.cm"}
    if program:
        result["program"] = PROGRAM
    if children:
        result["children"] = [
            child(entry) if isinstance(entry, str) else dict(child(entry[0]), manifest=f"//test:{entry[1]}.cm")
            for entry in children
        ]
    if manifest:
        result["manifest"] = manifest
    return result


def protocol(name: str, type: str = "") -> Dict[str, str]:
    """A `capabilities` or `use` clause for one protocol."""
    return dict({"protocol": name}, **({"type": type} if type else {}))


def offer(name: str, source: str, target: str, rename: str = "") -> Dict[str, str]:
    return dict(
        {"protocol": name, "from": source, "to": target}, **({"as": rename} if rename else {})
    )


def builtin(name: str, type: str = "", kind: int = cm.KIND_PROTOCOL) -> cb.Builtin:
    return cb.Builtin(name, kind, type or name, None)


def check(scopes: List[Dict[str, Any]], builtins: Optional[List[cb.Builtin]] = None) -> List[cm.Mismatch]:
    """check_types() for the first of |scopes| as the manifest being compiled
    and the rest as its descendants, as GN would hand them over."""
    root, *rest = scopes
    labels = [entry["manifest"] for entry in root.get("children", [])]
    meta = [{"label": label, "path": "meta/" + label.split(":")[-1]} for label in labels]
    decl = cm.compile_manifest(root, PROGRAM_NAMES if "program" in root else [], meta)
    return cm.check_types(root, decl, {entry["label"]: entry for entry in rest}, builtins)


def texts(mismatches: List[cm.Mismatch]) -> List[str]:
    return [cm.format_mismatch(mismatch) for mismatch in mismatches]


# The text the design accepted for the experiment of 2026-10-29: root.cm
# offered the built-in Launcher to #pc-ps2 as ours.resource.Irq, which is what
# pc-ps2 uses. The build error is exactly this.
EXPERIMENT_ERROR = """\
error: //kernel/products/image/dev:root.cm: #pc-ps2 uses protocol "ours.resource.Irq" (type ours.resource.Irq),
       but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
         use    #pc-ps2 "ours.resource.Irq"
         offer  root.cm "ours.process.Launcher" from parent to #pc-ps2 as "ours.resource.Irq"
       Offer a capability of type "ours.resource.Irq" under that name, or give the use the type it means.
"""


def server_scope(
    name: str = "server", capability: Optional[Dict[str, str]] = None, **extra: Any
) -> Dict[str, Any]:
    """A component with a program that provides and exposes one protocol,
    ours.example.Echo unless |capability| says another."""
    capability = capability or protocol("ours.example.Echo")
    return scope_of(
        name,
        program=True,
        capabilities=[capability],
        expose=[{"protocol": capability["protocol"], "from": "self"}],
        **extra,
    )


def client_scope(use: Dict[str, str], name: str = "client") -> Dict[str, Any]:
    """A component with a program that uses one protocol."""
    return scope_of(name, program=True, use=[use])


def experiment_scopes() -> List[Dict[str, Any]]:
    root = scope_of(
        "root",
        children=("pc-ps2",),
        offer=[offer("ours.process.Launcher", "parent", "#pc-ps2", "ours.resource.Irq")],
    )
    root["label"] = "//kernel/products/image/dev:root.cm"
    pc_ps2 = scope_of(
        "pc-ps2", program=True, use=[protocol("ours.resource.Irq"), protocol("ours.resource.Ioport")]
    )
    return [root, pc_ps2]


class TypeCheckTest(unittest.TestCase):
    """Each rule of the walk and of the verdict, on trees of a few manifests."""

    def test_the_experiment_of_2026_10_29_is_a_build_error(self) -> None:
        mismatches = check(experiment_scopes(), [builtin("ours.process.Launcher")])
        self.assertEqual(texts(mismatches), [EXPERIMENT_ERROR])

    def test_a_provider_that_declares_another_type_is_a_build_error(self) -> None:
        # Four hops, through two exposes: client's Hello is root's Service
        # from #svc, which svc takes from #impl's Echo, which impl exposes
        # from itself.
        root = scope_of(
            "root",
            children=("svc", "client"),
            offer=[offer("ours.example.Service", "#svc", "#client", "ours.example.Hello")],
        )
        svc = scope_of(
            "svc",
            children=("impl",),
            expose=[
                {"protocol": "ours.example.Echo", "from": "#impl", "as": "ours.example.Service"}
            ],
        )
        impl = server_scope("impl")
        client = client_scope(protocol("ours.example.Hello"))
        self.assertEqual(
            texts(check([root, svc, impl, client])),
            [
                """\
error: //test:root.cm: #client uses protocol "ours.example.Hello" (type ours.example.Hello),
       but the route ends at #svc/#impl's capability "ours.example.Echo" (type ours.example.Echo):
         use    #client "ours.example.Hello"
         offer  root.cm "ours.example.Service" from #svc to #client as "ours.example.Hello"
         expose #svc "ours.example.Echo" from #impl to parent as "ours.example.Service"
         expose #svc/#impl "ours.example.Echo" from self to parent
       Offer a capability of type "ours.example.Hello" under that name, or give the use the type it means.
"""
            ],
        )

    def test_a_capability_that_states_a_type_is_compared_by_it(self) -> None:
        # The capability's own name is not its type once it states another.
        root = scope_of("root", children=("server", "client"), offer=[offer("ours.example.Echo", "#server", "#client")])
        server = server_scope(capability=protocol("ours.example.Echo", "ours.example.EchoV2"))
        client = client_scope(protocol("ours.example.Echo"))
        (mismatch,) = check([root, server, client])
        self.assertEqual((mismatch.want, mismatch.resolved.end.type), ("ours.example.Echo", "ours.example.EchoV2"))
        self.assertIn('(type ours.example.EchoV2):', cm.format_mismatch(mismatch))

    def test_a_matching_route_builds(self) -> None:
        root = scope_of("root", children=("server", "client"), offer=[offer("ours.example.Echo", "#server", "#client")])
        self.assertEqual(check([root, server_scope(), client_scope(protocol("ours.example.Echo"))]), [])

    def test_a_renamed_use_with_its_type_stated_passes(self) -> None:
        root = scope_of("root", children=("server", "client"), offer=[offer("ours.example.Echo", "#server", "#client", "echo")])
        server, stated, bare = server_scope(), client_scope(protocol("echo", "ours.example.Echo")), client_scope(protocol("echo"))
        self.assertEqual(check([root, server, stated]), [])
        # Without the type the same route is the mismatch the type is for.
        (mismatch,) = check([root, server, bare])
        self.assertEqual((mismatch.want, mismatch.resolved.end.type), ("echo", "ours.example.Echo"))

    def test_a_route_through_two_offers_and_an_expose_is_followed(self) -> None:
        # root offers server's Echo to mid; mid offers it on to its child leaf
        # as `echo`; the server exposes it from itself.
        root = scope_of("root", children=("server", "mid"), offer=[offer("ours.example.Echo", "#server", "#mid")])
        mid = scope_of("mid", children=("leaf",), offer=[offer("ours.example.Echo", "parent", "#leaf", "echo")])
        server = server_scope()
        leaf = client_scope(protocol("echo", "ours.example.Echo"), "leaf")
        self.assertEqual(check([root, server, mid, leaf]), [])

        bare = client_scope(protocol("echo"), "leaf")
        (mismatch,) = check([root, server, mid, bare])
        self.assertEqual(mismatch.instance.path, ("mid", "leaf"))
        self.assertEqual(
            mismatch.resolved.hops,
            [
                'use    #mid/#leaf "echo"',
                'offer  #mid "ours.example.Echo" from parent to #leaf as "echo"',
                'offer  root.cm "ours.example.Echo" from #server to #mid',
                'expose #server "ours.example.Echo" from self to parent',
            ],
        )

    def test_a_route_that_ends_at_a_capability_serves_it_from_self(self) -> None:
        # An offer from self, with a program to serve it.
        root = scope_of("root", children=("client",), program=True, capabilities=[protocol("ours.example.Echo")], offer=[offer("ours.example.Echo", "self", "#client", "echo")])
        client = client_scope(protocol("echo", "ours.example.Echo"))
        self.assertEqual(check([root, client]), [])
        (mismatch,) = check([root, client_scope(protocol("echo"))])
        self.assertIn("root.cm's capability \"ours.example.Echo\"", cm.format_mismatch(mismatch))

    def test_the_declaration_found_is_the_one_the_walk_names(self) -> None:
        # Declared ahead of the one routed to, and of another type: the walk
        # takes the capability of its name, not the first.
        server = scope_of(
            "server",
            program=True,
            capabilities=[protocol("ours.example.First", "ours.example.Other"), protocol("ours.example.Echo")],
            expose=[{"protocol": "ours.example.Echo", "from": "self"}],
        )
        root = scope_of("root", children=("server", "client"), offer=[offer("ours.example.Echo", "#server", "#client")])
        self.assertEqual(check([root, server, client_scope(protocol("ours.example.Echo"))]), [])

    def test_a_route_to_a_builtin_is_compared_by_its_type(self) -> None:
        root = scope_of("root", children=("client",), offer=[offer("ours.process.Launcher", "parent", "#client")])
        client = client_scope(protocol("ours.process.Launcher"))
        self.assertEqual(check([root, client], [builtin("ours.process.Launcher")]), [])
        # A built-in that states a type is compared by it.
        (mismatch,) = check([root, client], [builtin("ours.process.Launcher", "ours.example.Echo")])
        self.assertEqual((mismatch.want, mismatch.resolved.end.type), ("ours.process.Launcher", "ours.example.Echo"))
        # Stating its own name is stating nothing.
        self.assertEqual(check([root, client], [builtin("ours.process.Launcher", "ours.process.Launcher")]), [])

    def test_a_use_in_the_root_itself_reaches_the_builtins(self) -> None:
        root = scope_of("root", use=[protocol("ours.process.Launcher", "ours.example.Echo")])
        (mismatch,) = check([root], [builtin("ours.process.Launcher")])
        self.assertEqual(
            cm.format_mismatch(mismatch),
            """\
error: //test:root.cm: root.cm uses protocol "ours.process.Launcher" (type ours.example.Echo),
       but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
         use    root.cm "ours.process.Launcher"
       Offer a capability of type "ours.example.Echo" under that name, or give the use the type it means.
""",
        )

    def test_a_manifest_under_two_parents_is_two_instances(self) -> None:
        # x.cm is both #a and #b: root offers the wrong protocol to #a only.
        root = scope_of(
            "root",
            children=(("a", "x"), ("b", "x"), "server"),
            offer=[
                offer("ours.example.Echo", "#server", "#a", "ours.example.Other"),
                offer("ours.example.Other", "parent", "#b"),
            ],
        )
        x = client_scope(protocol("ours.example.Other"), "x")
        mismatches = check([root, x, server_scope()], [builtin("ours.example.Other")])
        # #a reaches the server's Echo under the name `Other`: a mismatch.
        # #b reaches the built-in `Other` of that type: none.
        self.assertEqual([m.instance.path for m in mismatches], [("a",)])

    def test_every_mismatch_is_reported_by_instance_path_then_use(self) -> None:
        root = scope_of(
            "root",
            children=("b", "a", "server"),
            offer=[
                offer("ours.example.Echo", "#server", "#a", "one"),
                offer("ours.example.Echo", "#server", "#a", "two"),
                offer("ours.example.Echo", "#server", "#b", "three"),
            ],
        )
        a = scope_of("a", program=True, use=[protocol("two"), protocol("one")])
        b = scope_of("b", program=True, use=[protocol("three")])
        mismatches = check([root, b, a, server_scope()])
        self.assertEqual(
            [(m.instance.path, m.use.source_name) for m in mismatches],
            [(("a",), "two"), (("a",), "one"), (("b",), "three")],
        )
        self.assertEqual(len(texts(mismatches)), 3)

    def test_a_route_that_does_not_resolve_is_left_to_run_time(self) -> None:
        # Each use states a type nothing could have, so any of these routes
        # that did resolve would be a mismatch. None does, and the build goes
        # on, as routing_checks' deliberate failures need.
        use = protocol("ours.example.Echo", "ours.example.Wrong")
        to_client = [offer("ours.example.Echo", "#server", "#client")]
        above = scope_of("root", children=("client",), offer=[offer("ours.example.Echo", "parent", "#client")])
        silent = scope_of("server", program=True)
        hollow = scope_of(
            "server",
            capabilities=[protocol("ours.example.Echo")],
            expose=[{"protocol": "ours.example.Echo", "from": "self"}],
        )
        to_other = scope_of(
            "root",
            children=("client", "server", "other"),
            offer=[offer("ours.example.Echo", "#server", "#other")],
        )
        directory = {"directory": "ours.example.Echo", "from": "parent", "to": "#client", "rights": ["r*"]}
        cases = {
            "offered by nobody": ([scope_of("root", children=("client",)), client_scope(use)], None),
            "the child exposes nothing": (
                [scope_of("root", children=("client", "server"), offer=to_client), client_scope(use), silent],
                None,
            ),
            "offered from self, which has no program": (
                [
                    scope_of(
                        "root",
                        children=("client",),
                        capabilities=[protocol("ours.example.Echo")],
                        offer=[offer("ours.example.Echo", "self", "#client")],
                    ),
                    client_scope(use),
                ],
                None,
            ),
            "the provider has no program": (
                [scope_of("root", children=("client", "server"), offer=to_client), client_scope(use), hollow],
                None,
            ),
            "offered to another child": (
                [to_other, client_scope(use), server_scope(), scope_of("other", program=True)],
                None,
            ),
            "an offer of another kind": (
                [scope_of("root", children=("client",), offer=[directory]), client_scope(use)],
                [builtin("ours.example.Echo")],
            ),
            "above the top, with no built-ins given": ([above, client_scope(use)], None),
            "above the top, built-ins that lack it": ([above, client_scope(use)], [builtin("ours.process.Launcher")]),
            "above the top, a built-in of another kind": (
                [above, client_scope(use)],
                [builtin("ours.example.Echo", kind=cm.KIND_DIRECTORY)],
            ),
        }
        for name, (scopes, builtins) in cases.items():
            with self.subTest(name):
                self.assertEqual(check(scopes, builtins), [])

        # The same last route does resolve, and mismatch, once the list has it.
        self.assertEqual(len(check([above, client_scope(use)], [builtin("ours.example.Echo")])), 1)

    def test_the_rules_for_what_the_compiler_never_writes(self) -> None:
        # No manifest the compiler accepts exposes from the parent or declares
        # a directory, so the walk is given instances by hand. route() refuses
        # to follow the first, which would turn the walk back up, and matches
        # the second by kind; so does the walk.
        use = cm.Use(cm.KIND_PROTOCOL, "p", "/svc/p", "", None, "t")
        offer_p = cm.Offer(cm.KIND_PROTOCOL, cm.Ref(cm.REF_CHILD, "server"), "p", cm.Ref(cm.REF_CHILD, "client"), "p", "", None)

        def walk(expose_source: cm.Ref, capability_kind: int) -> Optional[cm.Resolved]:
            root = cm.Instance("//test:root.cm", (), cm.Decl(offers=[offer_p]), False)
            server_decl = cm.Decl(
                capabilities=[cm.Capability(capability_kind, "p", "other")],
                exposes=[cm.Expose(cm.KIND_PROTOCOL, expose_source, "p", "p")],
            )
            server = cm.Instance("//test:server.cm", ("server",), server_decl, True, root)
            client = cm.Instance("//test:client.cm", ("client",), cm.Decl(uses=[use]), True, root)
            root.children = {"server": server, "client": client}
            return cm.route_protocol_use(client, use, None, "root.cm")

        self.assertIsNotNone(walk(cm.Ref(cm.REF_SELF), cm.KIND_PROTOCOL))
        self.assertIsNone(walk(cm.Ref(cm.REF_PARENT), cm.KIND_PROTOCOL))
        self.assertIsNone(walk(cm.Ref(cm.REF_SELF), cm.KIND_DIRECTORY))

    def test_a_directory_use_is_not_compared(self) -> None:
        # `config` is the built-in `boot` under another name: by name alone
        # that would be a mismatch, and a directory is held to its rights.
        root = scope_of(
            "root",
            children=("client",),
            offer=[{"directory": "boot", "from": "parent", "to": "#client", "as": "config", "rights": ["r*"]}],
        )
        client = scope_of("client", program=True, use=[{"directory": "config", "path": "/config", "rights": ["r*"]}])
        boot = builtin("boot", kind=cm.KIND_DIRECTORY)
        self.assertEqual(check([root, client], [boot]), [])

    def test_a_tree_with_no_children_has_nothing_to_check(self) -> None:
        self.assertEqual(check([client_scope(protocol("anything", "else"))]), [])

    def test_a_missing_descendant_is_a_broken_build(self) -> None:
        root = scope_of("root", children=("client",))
        with self.assertRaisesRegex(cm.ManifestError, r"#client is //test:client.cm, which is not in the tree"):
            check([root])

    def test_a_manifest_that_is_its_own_ancestor_is_refused(self) -> None:
        root = scope_of("root", children=("loop",))
        loop = scope_of("loop", children=("loop",))
        with self.assertRaisesRegex(cm.ManifestError, "which is its own ancestor"):
            check([root, loop])

    def test_a_descendant_is_read_by_the_rules_of_a_manifest(self) -> None:
        root = scope_of("root", children=("client",))
        bad = scope_of("client", bogus=1)
        with self.assertRaisesRegex(cm.ManifestError, "unknown key 'bogus'"):
            check([root, bad])

    def test_the_tree_is_a_list_of_distinct_labels(self) -> None:
        with self.assertRaisesRegex(cm.ManifestError, "expected a list"):
            cm.load_tree({})
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "a.json"
            path.write_text("{}")
            entry = {"label": "//test:a.cm", "source": str(path)}
            sources, read = cm.load_tree([entry])
            self.assertEqual((sources, read), ({"//test:a.cm": {}}, [str(path)]))
            with self.assertRaisesRegex(cm.ManifestError, "is in the tree twice"):
                cm.load_tree([entry, entry])
            with self.assertRaisesRegex(cm.ManifestError, "unknown key 'path'"):
                cm.load_tree([dict(entry, path="x")])


class TypeCheckMainTest(unittest.TestCase):
    """The compiler's command line: what it prints, writes and exits with."""

    def run_compile(
        self,
        scopes: List[Dict[str, Any]],
        builtins: Optional[Dict[str, Any]] = None,
        extra: tuple = (),
        depfile: bool = False,
    ) -> tuple:
        root, *rest = scopes
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            tree = []
            for entry in rest:
                path = base / (entry["label"].split(":")[-1] + ".source.json")
                path.write_text(json.dumps(entry))
                tree.append({"label": entry["label"], "source": str(path)})
            labels = [entry["manifest"] for entry in root.get("children", [])]
            meta = [{"label": label, "path": "meta/" + label.split(":")[-1]} for label in labels]
            names = PROGRAM_NAMES if "program" in root else []
            arguments = [*extra]
            if builtins is not None:
                (base / "builtins.json").write_text(json.dumps(builtins))
                arguments += ["--builtins", str(base / "builtins.json")]
            depfile_path = base / "test.cm.d"
            if depfile:
                arguments += ["--depfile", str(depfile_path)]
            output = base / "out" / "test.cm"
            files = {}
            for key, value in [("source", root), ("program", names), ("children", meta), ("tree", tree)]:
                files[key] = base / f"{key}.json"
                files[key].write_text(json.dumps(value))
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                status = cm.main(
                    [
                        "--source", str(files["source"]),
                        "--program", str(files["program"]),
                        "--children", str(files["children"]),
                        "--tree", str(files["tree"]),
                        "--output", str(output),
                        *arguments,
                    ]
                )
            data = output.read_bytes() if output.exists() else None
            deps = depfile_path.read_text() if depfile_path.exists() else None
            return status, data, errors.getvalue(), deps, str(output), [str(base / (e["label"].split(":")[-1] + ".source.json")) for e in rest]

    LAUNCHER = {"builtins": [{"name": "ours.process.Launcher", "kind": "protocol"}]}

    def test_the_experiment_fails_the_build_with_the_designed_text(self) -> None:
        status, data, errors, *_ = self.run_compile(experiment_scopes(), self.LAUNCHER)
        self.assertEqual(status, 1)
        self.assertIsNone(data)
        self.assertEqual(errors, EXPERIMENT_ERROR)

    def test_the_builtins_file_gives_the_builtins_their_types(self) -> None:
        stated = {"builtins": [{"name": "ours.process.Launcher", "kind": "protocol", "type": "ours.resource.Irq"}]}
        # The Launcher stated as ours.resource.Irq is what #pc-ps2 means.
        status, data, errors, *_ = self.run_compile(experiment_scopes(), stated)
        self.assertEqual((status, errors), (0, ""))
        self.assertIsNotNone(data)

    def test_without_a_builtins_file_a_route_above_the_tree_is_not_checked(self) -> None:
        status, data, errors, *_ = self.run_compile(experiment_scopes())
        self.assertEqual((status, errors), (0, ""))
        self.assertIsNotNone(data)

    def test_no_type_check_skips_the_check(self) -> None:
        status, data, errors, *_ = self.run_compile(experiment_scopes(), self.LAUNCHER, ("--no-type-check",))
        self.assertEqual((status, errors), (0, ""))
        # The manifest is the one the checked build would have written.
        self.assertEqual(Reader(data).total_size, len(data))

    def test_no_type_check_does_not_read_the_tree(self) -> None:
        # A tree entry that names a file that is not there: unread.
        root, pc_ps2 = experiment_scopes()
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            for key, value in [
                ("source", root),
                ("program", []),
                ("children", [{"label": "//test:pc-ps2.cm", "path": "meta/pc-ps2.cm"}]),
                ("tree", [{"label": "//test:pc-ps2.cm", "source": str(base / "gone.json")}]),
            ]:
                (base / f"{key}.json").write_text(json.dumps(value))
            root["children"] = [dict(child("pc-ps2"))]
            (base / "source.json").write_text(json.dumps(root))
            arguments = [f"--{key}={base / (key + '.json')}" for key in ["source", "program", "children", "tree"]]
            arguments += [f"--output={base / 'test.cm'}", "--no-type-check"]
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(cm.main(arguments), 0)

    def test_every_mismatch_is_printed_before_the_build_fails(self) -> None:
        root = scope_of(
            "root",
            children=("client", "server"),
            offer=[offer("ours.example.Echo", "#server", "#client", "one"), offer("ours.example.Echo", "#server", "#client", "two")],
        )
        client = scope_of("client", program=True, use=[protocol("one"), protocol("two")])
        server = server_scope()
        status, data, errors, *_ = self.run_compile([root, client, server])
        self.assertEqual(status, 1)
        self.assertEqual(errors.count("error: //test:root.cm: "), 2)
        self.assertLess(errors.index('uses protocol "one"'), errors.index('uses protocol "two"'))
        # One message per mismatch, each ending in its fix.
        self.assertEqual(errors.count("or give the use the type it means.\n"), 2)

    def test_an_ordinary_refusal_keeps_its_prefix(self) -> None:
        status, _, errors, *_ = self.run_compile([scope_of("root", bogus=1)])
        self.assertEqual(status, 1)
        self.assertTrue(errors.startswith("component_manifest: "), errors)

    def test_a_bad_builtins_file_is_refused_by_name(self) -> None:
        status, data, errors, *_ = self.run_compile(experiment_scopes(), {"builtins": [{"name": "a"}]})
        self.assertEqual(status, 1)
        self.assertIsNone(data)
        self.assertIn("builtins[0]: needs `kind`", errors)

    def test_the_depfile_names_every_descendants_scope(self) -> None:
        root = scope_of("root", children=("mid", "server"), offer=[offer("ours.example.Echo", "#server", "#mid")])
        mid = scope_of("mid", children=("leaf",), offer=[offer("ours.example.Echo", "parent", "#leaf")])
        leaf = client_scope(protocol("ours.example.Echo"), "leaf")
        server = server_scope()
        status, data, errors, deps, output, sources = self.run_compile([root, mid, leaf, server], depfile=True)
        self.assertEqual((status, errors), (0, ""))
        self.assertEqual(deps, f"{output}: {' '.join(sources)}\n")
        # Every descendant, the grandchild too.
        self.assertEqual(len(sources), 3)
        self.assertTrue(any(path.endswith("leaf.cm.source.json") for path in sources))

    def test_a_failed_check_writes_no_depfile(self) -> None:
        status, _, _, deps, *_ = self.run_compile(experiment_scopes(), self.LAUNCHER, depfile=True)
        self.assertEqual(status, 1)
        self.assertIsNone(deps)

    def test_a_space_in_a_path_is_escaped_in_the_depfile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "d.d"
            cm._write_depfile(str(path), "out dir/x.cm", ["a b/c.json", "d.json"])
            self.assertEqual(path.read_text(), "out\\ dir/x.cm: a\\ b/c.json d.json\n")


# The fixtures route_test.cpp routes through, as GN wrote their scopes. Named,
# not found by a glob: mismatch.cm is built to fail the check and must not be
# taken for a tree that resolves.
FIXTURES = "//system/lib/component/test/fixtures"
RESOLVING_TREES = ["realm", "top"]
MISMATCHED_TREES = ["mismatch"]
FIXTURE_NAMES = ["realm", "provider", "consumer", "hollow", "top", "user", "leaf", "mismatch"]

# route_test.cpp's BUILTINS: boot, ours.process.Launcher and ours.resource.Debug.
ROUTE_TEST_BUILTINS = [
    builtin("boot", kind=cm.KIND_DIRECTORY),
    builtin("ours.process.Launcher"),
    builtin("ours.resource.Debug"),
]

# What route() answers for every protocol use in each tree, from the
# expectations of route_test.cpp: "<moniker> serves <name>" or
# "built-in <name>" for a route that resolves, and None for one that does not,
# whatever status and place route() reports. The TEST that decides it is named
# on each row. A use not listed here fails the test, so a fixture that gains a
# protocol use must say what route() makes of it.
EXPECTED_ROUTES: Dict[str, Dict[tuple, Optional[str]]] = {
    "realm": {
        # Route.ABuiltinProtocol
        (("consumer",), "ours.process.Launcher"): "built-in ours.process.Launcher",
        (("provider",), "ours.process.Launcher"): "built-in ours.process.Launcher",
        # Route.ASiblingThroughItsExpose
        (("consumer",), "echo"): "provider serves ours.example.Echo",
        # Route.AProviderWithoutAProgramIsNotFound
        (("consumer",), "ours.example.Hollow"): None,
    },
    "top": {
        # realm.cm's own routes, one level down: its Launcher offers come
        # from top's, which come from the built-ins.
        (("realm", "consumer"), "ours.process.Launcher"): "built-in ours.process.Launcher",
        (("realm", "provider"), "ours.process.Launcher"): "built-in ours.process.Launcher",
        (("realm", "consumer"), "echo"): "realm/provider serves ours.example.Echo",
        (("realm", "consumer"), "ours.example.Hollow"): None,
        # Route.TwoLevelsUpAndTwoDown
        (("user",), "ours.example.RealmEcho"): "realm/provider serves ours.example.Echo",
        (("user", "leaf"), "echo"): "realm/provider serves ours.example.Echo",
        # Route.AnOfferFromSelf
        (("user", "leaf"), "ours.example.UserEcho"): "user serves ours.example.UserEcho",
        # Route.NotFoundInAChildsExposes
        (("user",), "ours.example.Unexposed"): None,
        # Route.NotFoundAmongTheBuiltins
        (("user",), "ours.example.NotBuiltin"): None,
        # Route.AnOfferFromSelfWithoutAProgramIsNotFound
        (("user",), "ours.example.TopSelf"): None,
        # Route.OffersMatchTheKind
        (("user",), "ours.example.Kind"): None,
        # Route.BuiltinsMatchTheKind
        (("user",), "ours.example.BootProtocol"): None,
        # Route.OffersMatchTheTargetChild
        (("user",), "ours.example.ForRealm"): None,
        # Route.NotFoundInTheParentsOffers
        (("user",), "ours.example.Missing"): None,
    },
}


def find_build_directory() -> Optional[pathlib.Path]:
    """A build directory whose `gn gen` wrote the fixtures' scopes."""
    marker = "gen/system/lib/component/test/fixtures/top.cm.source.json"
    if os.environ.get("OURS_BUILD_DIR"):
        candidates = [pathlib.Path(os.environ["OURS_BUILD_DIR"])]
    else:
        candidates = sorted((pathlib.Path(__file__).resolve().parent.parent / "out").glob("*"))
    for candidate in candidates:
        if (candidate / marker).exists():
            return candidate
    return None


def fixture_scope(build: pathlib.Path, name: str) -> Dict[str, Any]:
    """The scope GN wrote for fixtures:<name>.cm, from the default toolchain's
    gen directory or the user one's: the same text in both."""
    pattern = f"gen/system/lib/component/test/fixtures/{name}.cm.source.json"
    for directory in [build, *sorted(build.glob("user-*"))]:
        path = directory / pattern if directory == build else directory / pattern.split("/", 1)[1]
        if path.exists():
            return json.loads(path.read_text())
    raise AssertionError(f"no scope written for {name}.cm under {build}: run gn gen")


class FixtureRoutesTest(unittest.TestCase):
    """The walk against route_test.cpp's: the same fixtures, the same routes."""

    @classmethod
    def setUpClass(cls) -> None:
        build = find_build_directory()
        if build is None:
            raise unittest.SkipTest(
                "no build directory has the fixtures' scopes: run `gn gen out/<dir>` "
                "(or set OURS_BUILD_DIR) and run this test again"
            )
        cls.scopes = {name: fixture_scope(build, name) for name in FIXTURE_NAMES}

    def tree(self, root: str) -> cm.Instance:
        scope = self.scopes[root]
        meta = [
            {"label": entry["manifest"], "path": "meta/x"} for entry in scope.get("children", [])
        ]
        decl = cm.compile_manifest(scope, PROGRAM_NAMES if "program" in scope else [], meta)
        sources = {entry["label"]: entry for entry in self.scopes.values()}
        return cm.build_tree(scope, decl, sources)

    def moniker(self, resolved: cm.Resolved) -> str:
        end = resolved.end
        if end.instance is None:
            return f"built-in {end.name}"
        return f"{'/'.join(end.instance.path)} serves {end.name}"

    def test_the_fixtures_are_the_ones_this_test_names(self) -> None:
        self.assertEqual(self.scopes["top"]["label"], f"{FIXTURES}:top.cm")
        self.assertEqual(sorted(RESOLVING_TREES + MISMATCHED_TREES), ["mismatch", "realm", "top"])

    def test_every_use_resolves_as_route_does(self) -> None:
        for name in RESOLVING_TREES:
            root = self.tree(name)
            found: Dict[tuple, Optional[str]] = {}
            for instance in cm.instances(root):
                for use in instance.decl.uses:
                    if use.kind != cm.KIND_PROTOCOL:
                        continue
                    resolved = cm.route_protocol_use(instance, use, ROUTE_TEST_BUILTINS, "root.cm")
                    found[(instance.path, use.source_name)] = self.moniker(resolved) if resolved else None
            self.assertEqual(found, EXPECTED_ROUTES[name], name)

    def test_the_expected_routes_are_not_all_failures(self) -> None:
        # A walk that resolved nothing would agree with a table of Nones.
        for name in RESOLVING_TREES:
            self.assertTrue(any(EXPECTED_ROUTES[name].values()), name)

    def test_every_route_that_resolves_ends_at_the_type_it_asks_for(self) -> None:
        for name in RESOLVING_TREES:
            scope = self.scopes[name]
            meta = [{"label": e["manifest"], "path": "meta/x"} for e in scope.get("children", [])]
            decl = cm.compile_manifest(scope, PROGRAM_NAMES if "program" in scope else [], meta)
            sources = {entry["label"]: entry for entry in self.scopes.values()}
            self.assertEqual(texts(cm.check_types(scope, decl, sources, ROUTE_TEST_BUILTINS)), [], name)
            # And without the built-ins: the same routes, fewer of them walked.
            self.assertEqual(texts(cm.check_types(scope, decl, sources, None)), [], name)

    def test_the_mismatched_tree_fails_on_both_sides(self) -> None:
        scope = self.scopes["mismatch"]
        meta = [{"label": e["manifest"], "path": "meta/x"} for e in scope["children"]]
        decl = cm.compile_manifest(scope, [], meta)
        sources = {entry["label"]: entry for entry in self.scopes.values()}
        found = [
            (m.instance.path, m.use.source_name, m.want, m.resolved.end.type, m.resolved.end.instance is None)
            for m in cm.check_types(scope, decl, sources, ROUTE_TEST_BUILTINS)
        ]
        self.assertEqual(
            found,
            [
                # In the order of consumer's uses. The built-in Launcher
                # offered as `echo`:
                (("consumer",), "echo", "ours.example.Echo", "ours.process.Launcher", True),
                # The provider's Echo offered as the Launcher:
                (("consumer",), "ours.process.Launcher", "ours.process.Launcher", "ours.example.Echo", False),
            ],
        )
        # With no list of built-ins only the provider's side can be seen.
        self.assertEqual(len(cm.check_types(scope, decl, sources, None)), 1)


if __name__ == "__main__":
    unittest.main()
