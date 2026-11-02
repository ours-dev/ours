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
import re
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

    def test_a_directory_capability_states_a_path_and_rights(self) -> None:
        manifest = {
            "capabilities": [
                {"directory": "data", "path": "/export/data", "rights": ["rw*"]},
                {"protocol": "p"},
                {"directory": "docs", "path": "/export/docs", "rights": ["read_bytes", "traverse"]},
            ]
        }
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual(
            decl.capabilities,
            [
                cm.Capability(cm.KIND_DIRECTORY, "data", "", "/export/data", cm.RIGHTS["rw*"]),
                cm.Capability(cm.KIND_PROTOCOL, "p"),
                cm.Capability(cm.KIND_DIRECTORY, "docs", "", "/export/docs", 0x02 | 0x80),
            ],
        )

    def test_a_directory_declaration_needs_a_path_and_rights(self) -> None:
        self.refuses(
            r"capabilities\[0\]: a directory declaration needs `path`",
            source({"capabilities": [{"directory": "data", "rights": ["r*"]}]}),
            [],
            [],
        )
        self.refuses(
            r"capabilities\[0\]: a directory declaration needs `rights`",
            source({"capabilities": [{"directory": "data", "path": "/export/data"}]}),
            [],
            [],
        )

    def test_a_directory_declaration_has_no_subdir_and_no_type(self) -> None:
        clause = {"directory": "data", "path": "/export/data", "rights": ["r*"]}
        self.refuses(
            r"capabilities\[0\]\.subdir: a declaration has no `subdir`",
            source({"capabilities": [dict(clause, subdir="shared")]}),
            [],
            [],
        )
        self.refuses(
            r"capabilities\[0\]\.type: a directory has no `type`",
            source({"capabilities": [dict(clause, type="t")]}),
            [],
            [],
        )
        # A protocol declaration has no subdir either.
        self.refuses(
            r"capabilities\[0\]\.subdir: a declaration has no `subdir`",
            source({"capabilities": [{"protocol": "p", "subdir": "shared"}]}),
            [],
            [],
        )

    def test_a_protocol_declaration_has_no_path_and_no_rights(self) -> None:
        for key, value in [("path", "/export/p"), ("rights", ["r*"])]:
            self.refuses(
                rf"capabilities\[0\]\.{key}: a protocol declaration has no `{key}`",
                source({"capabilities": [{"protocol": "p", key: value}]}),
                [],
                [],
            )

    def test_a_directory_declaration_names_one_directory(self) -> None:
        clause = {"directory": ["a", "b"], "path": "/export/data", "rights": ["r*"]}
        self.refuses(
            r"capabilities\[0\]\.directory: a declaration names one directory",
            source({"capabilities": [clause]}),
            [],
            [],
        )

    def test_a_declared_path_is_absolute_and_clean(self) -> None:
        for path in ["export/data", "/", "/export/", "/export//data", "/export/./data", "/export/../x"]:
            manifest = {"capabilities": [{"directory": "data", "path": path, "rights": ["r*"]}]}
            self.refuses(r"capabilities\[0\]\.path: ", source(manifest), [], [])
        longest = "/" + "a" * cm.MAX_SEGMENT_LENGTH
        manifest = {"capabilities": [{"directory": "data", "path": longest, "rights": ["r*"]}]}
        self.assertEqual(cm.compile_manifest(source(manifest), [], []).capabilities[0].path, longest)
        manifest = {"capabilities": [{"directory": "data", "path": longest + "a", "rights": ["r*"]}]}
        self.refuses("segment longer than 255 bytes", source(manifest), [], [])

    def test_a_declared_path_is_not_svc_or_under_it(self) -> None:
        for path in ["/svc", "/svc/x", "/svc/x/y"]:
            manifest = {"capabilities": [{"directory": "data", "path": path, "rights": ["r*"]}]}
            self.refuses(
                rf"capabilities\[0\]\.path: '{path}' is /svc or lies under or above it",
                source(manifest),
                [],
                [],
            )
        # By segments: /svcx and /svc2 are other directories.
        for path in ["/svcx", "/svc2/x", "/x/svc"]:
            manifest = {"capabilities": [{"directory": "data", "path": path, "rights": ["r*"]}]}
            self.assertEqual(cm.compile_manifest(source(manifest), [], []).capabilities[0].path, path)

    def test_declared_paths_do_not_overlap(self) -> None:
        def declare(*paths: str) -> Dict[str, Any]:
            return {
                "capabilities": [
                    {"directory": f"d{index}", "path": path, "rights": ["r*"]}
                    for index, path in enumerate(paths)
                ]
            }

        for paths in [
            ("/export/data", "/export/data"),
            ("/export/data", "/export/data/x"),
            ("/export/data/x", "/export/data"),
            ("/export", "/export/data"),
        ]:
            self.refuses(
                r"capabilities\[1\]\.path: '.*' overlaps '.*' \(.*capabilities\[0\]\.path\)",
                source(declare(*paths)),
                [],
                [],
            )
        # By segments: data2 is not under data, and two directories may share
        # the directory above them.
        decl = cm.compile_manifest(
            source(declare("/export/data", "/export/data2", "/export/a", "/other")), [], []
        )
        self.assertEqual(len(decl.capabilities), 4)

    def test_declared_rights_follow_the_rules_of_every_other_rights_clause(self) -> None:
        for rights, error in [
            (["read"], "unknown right 'read'"),
            (["r*", "r*"], "duplicated in the rights clause"),
            ([], "non-empty list"),
            ("r*", "non-empty list"),
        ]:
            manifest = {"capabilities": [{"directory": "d", "path": "/d", "rights": rights}]}
            self.refuses(rf"capabilities\[0\]\.rights: .*{error}", source(manifest), [], [])

    def test_a_directory_and_a_protocol_share_the_names_of_capabilities(self) -> None:
        manifest = {
            "capabilities": [
                {"protocol": "data"},
                {"directory": "data", "path": "/export/data", "rights": ["r*"]},
            ]
        }
        self.refuses("'data' is a duplicate capability name", source(manifest), [], [])
        manifest = {
            "capabilities": [
                {"directory": "data", "path": "/export/data", "rights": ["r*"]},
                {"directory": "data", "path": "/export/other", "rights": ["r*"]},
            ]
        }
        self.refuses("'data' is a duplicate capability name", source(manifest), [], [])

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

    def test_a_directory_offer_comes_from_the_parent_self_or_a_child(self) -> None:
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            "offer": [
                {"directory": "boot", "from": "parent", "to": "#a", "rights": ["rx*"]},
                {"directory": "data", "from": "self", "to": "#a", "rights": ["r*"], "subdir": "pub"},
                {"directory": "docs", "from": "#b", "to": "#a", "as": "notes", "rights": ["r*"]},
            ],
        }
        decl = compile_with_children(manifest, "a", "b")
        self.assertEqual(
            decl.offers,
            [
                cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_PARENT), "boot", cm.Ref(cm.REF_CHILD, "a"), "boot", "", cm.RIGHTS["rx*"]),
                cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "data", cm.Ref(cm.REF_CHILD, "a"), "data", "pub", cm.RIGHTS["r*"]),
                cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_CHILD, "b"), "docs", cm.Ref(cm.REF_CHILD, "a"), "notes", "", cm.RIGHTS["r*"]),
            ],
        )

    def test_a_directory_offer_from_self_names_a_declared_directory(self) -> None:
        offer = {"directory": "data", "from": "self", "to": "#a", "rights": ["r*"]}
        # Declared nowhere, and declared as a protocol: neither is a directory.
        for capabilities in [[], [{"protocol": "data"}]]:
            self.refuses(
                r"offer\[0\]\.from: 'self' offers 'data', which `capabilities` does not declare",
                source({"capabilities": capabilities, "offer": [offer]}, children=[child("a")]),
                [],
                child_meta("a"),
            )
        # And the reverse: a protocol offer from self of a declared directory.
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["r*"]}],
            "offer": [{"protocol": "data", "from": "self", "to": "#a"}],
        }
        self.refuses(
            "does not declare", source(manifest, children=[child("a")]), [], child_meta("a")
        )

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

    def test_the_cycle_check_covers_a_directory_offer_from_a_child(self) -> None:
        manifest = {
            "offer": [
                {"directory": "d", "from": "#a", "to": "#b", "rights": ["r*"]},
                {"protocol": "q", "from": "#b", "to": "#c"},
                {"directory": "r", "from": "#c", "to": "#a", "rights": ["r*"]},
            ]
        }
        self.refuses(
            r"cycle: #a -> #b -> #c -> #a",
            source(manifest, children=[child("a"), child("b"), child("c")]),
            [],
            child_meta("a", "b", "c"),
        )
        # Only the directory offers make this one, and a protocol offer cannot
        # hide it.
        manifest = {
            "offer": [
                {"directory": "d", "from": "#a", "to": "#b", "rights": ["r*"]},
                {"directory": "e", "from": "#b", "to": "#a", "rights": ["r*"]},
            ]
        }
        self.refuses(
            r"cycle: #a -> #b -> #a",
            source(manifest, children=[child("a"), child("b")]),
            [],
            child_meta("a", "b"),
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

    def test_a_directory_expose_from_self_or_a_child(self) -> None:
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            "expose": [
                {"directory": "data", "from": "self", "rights": ["r*"], "subdir": "shared"},
                {"directory": "data", "from": "self", "as": "plain"},
                {"directory": "docs", "from": "#a", "as": "notes", "rights": ["rx*"], "to": "parent"},
            ],
        }
        decl = cm.compile_manifest(source(manifest, children=[child("a")]), [], child_meta("a"))
        self.assertEqual(
            decl.exposes,
            [
                cm.Expose(cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "data", "data", "shared", cm.RIGHTS["r*"]),
                cm.Expose(cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "data", "plain", "", None),
                cm.Expose(cm.KIND_DIRECTORY, cm.Ref(cm.REF_CHILD, "a"), "docs", "notes", "", cm.RIGHTS["rx*"]),
            ],
        )

    def test_a_directory_expose_from_self_names_a_declared_directory(self) -> None:
        for capabilities in [[], [{"protocol": "data"}]]:
            manifest = {"capabilities": capabilities, "expose": [{"directory": "data", "from": "self"}]}
            self.refuses(
                r"expose\[0\]\.from: 'self' exposes 'data', which `capabilities` does not declare",
                source(manifest),
                [],
                [],
            )
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["r*"]}],
            "expose": [{"protocol": "data", "from": "self"}],
        }
        self.refuses("does not declare", source(manifest), [], [])

    def test_one_declaration_may_be_exposed_twice(self) -> None:
        # Two exposes of one declaration under two names, each narrowing in
        # its own way: the only way to give a parent two views of one tree.
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            "expose": [
                {"directory": "data", "from": "self", "rights": ["r*"], "subdir": "pub", "as": "view"},
                {"directory": "data", "from": "self"},
            ],
        }
        self.assertEqual(len(cm.compile_manifest(source(manifest), [], []).exposes), 2)

    def test_rights_and_subdir_are_for_directory_exposes(self) -> None:
        for key, value in [("rights", ["r*"]), ("subdir", "shared")]:
            manifest = {
                "capabilities": [{"protocol": "p"}],
                "expose": [{"protocol": "p", "from": "self", key: value}],
            }
            self.refuses(
                rf"expose\[0\]\.{key}: only a directory expose has `{key}`", source(manifest), [], []
            )
            # Nor does a protocol expose of a child's capability.
            manifest = {"expose": [{"protocol": "p", "from": "#a", key: value}]}
            self.refuses(
                rf"expose\[0\]\.{key}: only a directory expose has `{key}`",
                source(manifest, children=[child("a")]),
                [],
                child_meta("a"),
            )

    def test_the_rights_and_subdir_of_an_expose_follow_the_rules_of_an_offers(self) -> None:
        clause = {"directory": "data", "from": "#a"}
        for key, value, error in [
            ("rights", ["read"], "unknown right 'read'"),
            ("rights", ["r*", "r*"], "duplicated in the rights clause"),
            ("rights", [], "non-empty list"),
            ("subdir", "/shared", "must be relative"),
            ("subdir", "shared/", "empty, '.' or '..' segment"),
            ("subdir", "a/../b", "empty, '.' or '..' segment"),
            ("subdir", "", "empty, '.' or '..' segment"),
        ]:
            self.refuses(
                rf"expose\[0\]\.{key}: .*{error}",
                source({"expose": [dict(clause, **{key: value})]}, children=[child("a")]),
                [],
                child_meta("a"),
            )

    def test_the_language_is_opened_for_directories(self) -> None:
        # What step 1 left refused (format 3 had the room, the compiler did
        # not write it) is the language now: a declared directory with a path
        # and rights, an exposed directory with a subdir and rights, and a
        # directory offer from self and from a child, each in the full
        # spelling the design uses.
        decl = cm.compile_manifest(
            source({"capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}]}),
            [],
            [],
        )
        self.assertEqual(decl.capabilities[0].path, "/export/data")
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            "expose": [{"directory": "data", "from": "self", "rights": ["r*"], "subdir": "shared"}],
        }
        decl = cm.compile_manifest(source(manifest), [], [])
        self.assertEqual((decl.exposes[0].subdir, decl.exposes[0].rights), ("shared", cm.RIGHTS["r*"]))
        for origin in ["self", "#b"]:
            manifest = {
                "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
                "offer": [{"directory": "data", "from": origin, "to": "#a", "rights": ["r*"]}],
            }
            decl = compile_with_children(manifest, "a", "b")
            self.assertEqual(decl.offers[0].kind, cm.KIND_DIRECTORY)

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
        ]:
            self.refuses(
                r"offer\[0\]\.type: an offer has no `type`: `as` renames the capability, not the protocol",
                source({"offer": [offer]}, children=[child("a")]),
                [],
                child_meta("a"),
            )
        # A directory has no type to state anywhere: the message says so,
        # and does not send the author to declare one.
        for origin in ["parent", "self", "#b"]:
            offer = {"directory": "boot", "from": origin, "to": "#a", "type": "t"}
            self.refuses(
                r"offer\[0\]\.type: a directory offer has no `type`; only a protocol has one",
                source({"offer": [offer]}, children=[child("a"), child("b")]),
                [],
                child_meta("a", "b"),
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
        # And a directory expose has none to state, from self or a child.
        for origin in ["self", "#a"]:
            manifest = {
                "capabilities": [{"directory": "d", "path": "/d", "rights": ["r*"]}],
                "expose": [{"directory": "d", "from": origin, "type": "t"}],
            }
            self.refuses(
                r"expose\[0\]\.type: a directory expose has no `type`; only a protocol has one",
                source(manifest, children=[child("a")]),
                [],
                child_meta("a"),
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
            (cm.CAPABILITY, 40),
            (cm.USE, 56),
            (cm.OFFER, 56),
            (cm.EXPOSE, 56),
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

    def test_the_format_is_version_three(self) -> None:
        self.assertEqual(cm.VERSION, 3)
        data = self.compiled()
        self.assertEqual(Reader(data).version, 3)
        # The header's version is a u16 at offset 4, read here without the
        # struct that wrote it.
        self.assertEqual(struct.unpack_from("<H", data, 4), (3,))

    def test_the_fields_sit_where_format_hpp_puts_them(self) -> None:
        # offsetof(Capability, flags) == 4, name == 8, type == 16, path == 24
        # and rights == 32; offsetof(Use, type) == 40 and rights == 48;
        # offsetof(Expose, target) == 24, subdir == 40 and rights == 48.
        capability = cm.CAPABILITY.pack(*range(1, 9), 99)
        self.assertEqual(struct.unpack_from("<I", capability, 4), (2,))
        self.assertEqual(struct.unpack_from("<II", capability, 8), (3, 4))
        self.assertEqual(struct.unpack_from("<II", capability, 16), (5, 6))
        self.assertEqual(struct.unpack_from("<II", capability, 24), (7, 8))
        self.assertEqual(struct.unpack_from("<Q", capability, 32), (99,))
        use = cm.USE.pack(*range(1, 13), 99)
        self.assertEqual(struct.unpack_from("<II", use, 40), (11, 12))
        self.assertEqual(struct.unpack_from("<Q", use, 48), (99,))
        expose = cm.EXPOSE.pack(*range(1, 13), 99)
        self.assertEqual(struct.unpack_from("<I", expose, 4), (2,))
        self.assertEqual(struct.unpack_from("<II", expose, 24), (7, 8))
        self.assertEqual(struct.unpack_from("<II", expose, 40), (11, 12))
        self.assertEqual(struct.unpack_from("<Q", expose, 48), (99,))
        # An Expose has an Offer's layout, so a reader that handles one
        # handles the other.
        self.assertEqual(cm.EXPOSE.format, cm.OFFER.format)

    def test_existing_capabilities_and_exposes_encode_the_new_fields_empty(self) -> None:
        # Every capability and expose the language can write today: a
        # protocol states no path, no subdir and no rights.
        reader = Reader(self.compiled())
        (capability,) = reader.tables["capability"]
        kind, flags, *strings, rights = capability
        self.assertEqual((kind, flags, rights), (cm.KIND_PROTOCOL, 0, 0))
        self.assertEqual(strings[4:6][1], 0)  # the path, empty
        self.assertEqual(reader.text(*strings[0:2]), "ours.example.Echo")
        (expose,) = reader.tables["expose"]
        self.assertEqual((expose[0], expose[1], expose[12]), (cm.KIND_PROTOCOL, 0, 0))
        self.assertEqual(expose[10:12][1], 0)  # the subdir, empty
        self.assertEqual(reader.text(*expose[4:6]), "ours.example.Echo")
        # An empty string adds no byte to the table.
        self.assertEqual(reader.strings.count(b"ours.example.Echo"), 1)

    def test_a_directory_capability_and_expose_encode_at_the_offsets_format_hpp_gives(self) -> None:
        # Records built by hand, since the language cannot write them yet,
        # read back at the offsets format.hpp's static_asserts fix and without
        # the structs that wrote them: header 48, Capability 40, Expose 56.
        decl = cm.Decl(
            capabilities=[
                cm.Capability(cm.KIND_DIRECTORY, "data", "", "/export/data", cm.RIGHTS["rw*"]),
                cm.Capability(cm.KIND_DIRECTORY, "stated-empty", "", "/export/none", 0),
            ],
            exposes=[
                cm.Expose(
                    cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "data", "docs", "shared/x", cm.RIGHTS["r*"]
                ),
                cm.Expose(cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "data", "plain"),
            ],
        )
        data = cm.encode(decl)
        header = 48
        capabilities = header
        exposes = capabilities + 2 * 40
        strings = exposes + 2 * 56
        self.assertEqual(struct.unpack_from("<I", data, 8), (len(data),))
        self.assertEqual(struct.unpack_from("<I", data, 12), (len(data) - strings,))
        self.assertEqual(struct.unpack_from("<II", data, 28), (2, 0))  # capabilities, uses
        self.assertEqual(struct.unpack_from("<II", data, 36), (0, 2))  # offers, exposes

        def text(offset: int) -> str:
            start, size = struct.unpack_from("<II", data, offset)
            return data[strings + start : strings + start + size].decode()

        # data: kind, flags, name, type, path, rights.
        self.assertEqual(struct.unpack_from("<II", data, capabilities), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED))
        self.assertEqual(text(capabilities + 8), "data")
        self.assertEqual(text(capabilities + 16), "")
        self.assertEqual(text(capabilities + 24), "/export/data")
        self.assertEqual(struct.unpack_from("<Q", data, capabilities + 32), (cm.RIGHTS["rw*"],))
        # A stated empty set is the flag and a zero word, not an unstated one.
        second = capabilities + 40
        self.assertEqual(struct.unpack_from("<II", data, second), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED))
        self.assertEqual(text(second + 24), "/export/none")
        self.assertEqual(struct.unpack_from("<Q", data, second + 32), (0,))

        # docs: kind, flags, source, source_name, target, target_name, subdir, rights.
        self.assertEqual(struct.unpack_from("<II", data, exposes), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED))
        self.assertEqual(struct.unpack_from("<II", data, exposes + 8), (cm.REF_SELF, 0))
        self.assertEqual(text(exposes + 16), "data")
        self.assertEqual(struct.unpack_from("<II", data, exposes + 24), (cm.REF_PARENT, 0))
        self.assertEqual(text(exposes + 32), "docs")
        self.assertEqual(text(exposes + 40), "shared/x")
        self.assertEqual(struct.unpack_from("<Q", data, exposes + 48), (cm.RIGHTS["r*"],))
        # The second expose states neither.
        plain = exposes + 56
        self.assertEqual(struct.unpack_from("<II", data, plain), (cm.KIND_DIRECTORY, 0))
        self.assertEqual(text(plain + 32), "plain")
        self.assertEqual(text(plain + 40), "")
        self.assertEqual(struct.unpack_from("<Q", data, plain + 48), (0,))

        # The file reads back through the second reading of the format, too.
        reader = Reader(data)
        self.assertEqual(reader.tables["capability"][0][8], cm.RIGHTS["rw*"])
        self.assertEqual(reader.tables["expose"][0][12], cm.RIGHTS["r*"])

    def test_a_compiled_directory_writes_its_path_rights_subdir_and_flags(self) -> None:
        # The same records as the hand-built test above, but from a manifest:
        # what the parser fills in is what encode() writes, at the offsets
        # format.hpp fixes. Read without the structs that wrote them.
        manifest = {
            "capabilities": [
                {"directory": "data", "path": "/export/data", "rights": ["rw*"]},
                {"protocol": "p"},
            ],
            "expose": [
                {"directory": "data", "from": "self", "rights": ["r*"], "subdir": "shared"},
                {"directory": "data", "from": "self", "as": "plain"},
                {"protocol": "p", "from": "self"},
            ],
        }
        data = cm.encode(cm.compile_manifest(source(manifest), [], []))
        capabilities = 48
        exposes = capabilities + 2 * 40
        strings = exposes + 3 * 56

        def text(offset: int) -> str:
            start, size = struct.unpack_from("<II", data, offset)
            return data[strings + start : strings + start + size].decode()

        self.assertEqual(struct.unpack_from("<II", data, capabilities), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED))
        self.assertEqual(text(capabilities + 8), "data")
        self.assertEqual(text(capabilities + 16), "")
        self.assertEqual(text(capabilities + 24), "/export/data")
        self.assertEqual(struct.unpack_from("<Q", data, capabilities + 32), (cm.RIGHTS["rw*"],))
        # The protocol states no path and no rights, and no flag says it does.
        protocol = capabilities + 40
        self.assertEqual(struct.unpack_from("<II", data, protocol), (cm.KIND_PROTOCOL, 0))
        self.assertEqual(text(protocol + 24), "")
        self.assertEqual(struct.unpack_from("<Q", data, protocol + 32), (0,))

        self.assertEqual(struct.unpack_from("<II", data, exposes), (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED))
        self.assertEqual(text(exposes + 32), "data")
        self.assertEqual(text(exposes + 40), "shared")
        self.assertEqual(struct.unpack_from("<Q", data, exposes + 48), (cm.RIGHTS["r*"],))
        plain = exposes + 56
        self.assertEqual(struct.unpack_from("<II", data, plain), (cm.KIND_DIRECTORY, 0))
        self.assertEqual(text(plain + 32), "plain")
        self.assertEqual(text(plain + 40), "")
        self.assertEqual(struct.unpack_from("<Q", data, plain + 48), (0,))
        protocol_expose = exposes + 2 * 56
        self.assertEqual(struct.unpack_from("<II", data, protocol_expose), (cm.KIND_PROTOCOL, 0))
        self.assertEqual(text(protocol_expose + 40), "")
        self.assertEqual(struct.unpack_from("<Q", data, protocol_expose + 48), (0,))

        # The second reading of the format agrees, and the file is whole.
        reader = Reader(data)
        self.assertEqual(reader.total_size, len(data))
        self.assertEqual(reader.text(*reader.tables["capability"][0][6:8]), "/export/data")
        self.assertEqual(reader.text(*reader.tables["expose"][0][10:12]), "shared")

    def test_an_offer_from_self_or_a_child_writes_its_source(self) -> None:
        manifest = {
            "capabilities": [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            "offer": [
                {"directory": "data", "from": "self", "to": "#a", "rights": ["r*"], "subdir": "pub"},
                {"directory": "docs", "from": "#b", "to": "#a", "rights": ["r*"]},
            ],
        }
        decl = compile_with_children(manifest, "a", "b")
        offers = Reader(cm.encode(decl)).tables["offer"]
        self.assertEqual(offers[0][:4], (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED, cm.REF_SELF, 0))
        self.assertEqual(offers[1][:4], (cm.KIND_DIRECTORY, cm.FLAG_RIGHTS_STATED, cm.REF_CHILD, 1))
        self.assertEqual(offers[1][6:8], (cm.REF_CHILD, 0))

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
        self.assertEqual(struct.calcsize("<IIIIIIII"), 32)
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


def run_compile(
    scopes: List[Dict[str, Any]],
    builtins: Optional[Dict[str, Any]] = None,
    extra: tuple = (),
    depfile: bool = False,
) -> tuple:
    """cm.main() over |scopes|, the first the manifest being compiled and the rest its
    descendants, as GN would hand them over: the exit status, the file it wrote
    (None when it wrote none), what it printed to stderr, the depfile (None when
    none), the output path and the paths of the descendants' scopes."""
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


class TypeCheckMainTest(unittest.TestCase):
    """The compiler's command line: what it prints, writes and exits with."""

    run_compile = staticmethod(run_compile)

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


# --- The rights walk ----------------------------------------------------------


def boot_builtin(rights: Optional[str] = "rx*") -> cb.Builtin:
    """The root's built-in `boot`, stating |rights| as builtins.json does."""
    return cb.Builtin("boot", cm.KIND_DIRECTORY, "boot", cm.RIGHTS[rights] if rights else None)


def directory_offer(
    name: str,
    source: str,
    target: str,
    rights: Optional[str] = None,
    subdir: str = "",
    rename: str = "",
) -> Dict[str, Any]:
    clause: Dict[str, Any] = {"directory": name, "from": source, "to": target}
    if rights:
        clause["rights"] = [rights]
    if subdir:
        clause["subdir"] = subdir
    if rename:
        clause["as"] = rename
    return clause


def directory_use(name: str, path: str, rights: str, subdir: str = "") -> Dict[str, Any]:
    clause: Dict[str, Any] = {"directory": name, "path": path, "rights": [rights]}
    if subdir:
        clause["subdir"] = subdir
    return clause


def directory_tree(
    offered: Optional[str],
    used: str = "r*",
    declared: str = "rw*",
    exposed: Optional[Dict[str, Any]] = None,
) -> List[Dict[str, Any]]:
    """root.cm, a provider `store` and a consumer `reader`: store declares
    `data` at /export/data allowing |declared|, exposes it as |exposed| says
    (rights and subdir, none by default), root offers it to reader stating
    |offered|, and reader uses it asking |used|."""
    root = scope_of(
        "root",
        children=("store", "reader"),
        offer=[directory_offer("data", "#store", "#reader", offered)],
    )
    store = scope_of(
        "store",
        program=True,
        capabilities=[{"directory": "data", "path": "/export/data", "rights": [declared]}],
        expose=[dict({"directory": "data", "from": "self"}, **(exposed or {}))],
    )
    reader = scope_of("reader", program=True, use=[directory_use("data", "/data", used)])
    return [root, store, reader]


def check_rights_of(
    scopes: List[Dict[str, Any]], builtins: Optional[List[cb.Builtin]] = None
) -> List[cm.RightsWarning]:
    """check_directory_rights() for the first of |scopes| as the manifest being
    compiled and the rest as its descendants, as check() does for the types."""
    root, *rest = scopes
    labels = [entry["manifest"] for entry in root.get("children", [])]
    meta = [{"label": label, "path": "meta/" + label.split(":")[-1]} for label in labels]
    decl = cm.compile_manifest(root, PROGRAM_NAMES if "program" in root else [], meta)
    return cm.check_directory_rights(root, decl, {entry["label"]: entry for entry in rest}, builtins)


def warning_texts(warnings: List[cm.RightsWarning]) -> List[str]:
    return [cm.format_rights_warning(warning) for warning in warnings]


# routing_checks' `/denied`, as dev's root.cm writes it: the built-in `boot`
# offered with r* where the use asks rx*. The warning on every compile of that
# root.
DENIED_WARNING = """\
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
           use    #routing_checks "denied"                                    rx*
           offer  root.cm "boot" from parent to #routing_checks as "denied"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.
"""


def denied_scopes() -> List[Dict[str, Any]]:
    root = scope_of(
        "root",
        children=("routing_checks",),
        offer=[directory_offer("boot", "parent", "#routing_checks", "r*", rename="denied")],
    )
    root["label"] = "//kernel/products/image/dev:root.cm"
    routing_checks = scope_of(
        "routing_checks", program=True, use=[directory_use("denied", "/denied", "rx*")]
    )
    return [root, routing_checks]


class RightsWalkTest(unittest.TestCase):
    """Each rule of the walk and of the warning, on trees of a few manifests."""

    def test_a_widened_offer_warns_with_the_designed_text(self) -> None:
        self.assertEqual(warning_texts(check_rights_of(denied_scopes(), [boot_builtin()])), [DENIED_WARNING])

    def test_a_use_that_asks_more_than_the_offer_states_warns_at_the_offer(self) -> None:
        (warning,) = check_rights_of(directory_tree("r*", used="rx*"))
        self.assertEqual(warning.instance.path, ("reader",))
        self.assertEqual((warning.walk.hops[-1].word, warning.walk.hops[-1].verb), ("offer", "Offer"))
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: #reader uses directory "data" with rights rx*,
         but the route holds less at an offer:
           use    #reader "data"                          rx*
           offer  root.cm "data" from #store to #reader   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.
""",
        )

    def test_an_exposed_narrowing_then_an_offer_asking_more_warns_at_the_expose(self) -> None:
        # store exposes r* what it declares rw*; root offers rx*, which reader
        # asks: the offer is as wide as the use, and it is the expose that
        # holds less than the offer in front of it.
        (warning,) = check_rights_of(directory_tree("rx*", used="rx*", exposed={"rights": ["r*"]}))
        self.assertEqual((warning.walk.hops[-1].word, warning.walk.hops[-1].verb), ("expose", "Expose"))
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: #reader uses directory "data" with rights rx*,
         but the route holds less at an expose:
           use    #reader "data"                          rx*
           offer  root.cm "data" from #store to #reader   rx*
           expose #store "data" from self to parent       r*   <- holds less
         The root will answer AccessDenied for this use. Expose rights that contain rx*, or ask the offer for fewer.
""",
        )

    def test_an_expose_wider_than_the_declaration_warns_at_the_declaration(self) -> None:
        # rx* adds execute to what rw* allows, so the declaration is the hop
        # that holds less, though both are "more than read".
        (warning,) = check_rights_of(
            directory_tree("rx*", used="rx*", declared="rw*", exposed={"rights": ["rx*"]})
        )
        self.assertEqual((warning.walk.hops[-1].word, warning.walk.hops[-1].verb), ("declare", "Declare"))
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: #reader uses directory "data" with rights rx*,
         but the route holds less at the declaration:
           use     #reader "data"                          rx*
           offer   root.cm "data" from #store to #reader   rx*
           expose  #store "data" from self to parent       rx*
           declare #store "data" at /export/data           rw*   <- holds less
         The root will answer AccessDenied for this use. Declare rights that contain rx*, or ask the expose for fewer.
""",
        )

    def test_a_declaration_narrower_than_an_offer_that_exposes_nothing_warns_at_the_declaration(self) -> None:
        (warning,) = check_rights_of(directory_tree("rx*", used="r*", declared="r*"))
        self.assertEqual((warning.walk.hops[-1].word, warning.walk.hops[-1].rights), ("declare", cm.RIGHTS["r*"]))
        self.assertIn("ask the offer for fewer.", cm.format_rights_warning(warning))

    def test_a_use_wider_than_the_builtin_warns_at_the_builtin(self) -> None:
        root = scope_of(
            "root", children=("reader",), offer=[directory_offer("boot", "parent", "#reader")]
        )
        reader = scope_of("reader", program=True, use=[directory_use("boot", "/boot", "rw*")])
        (warning,) = check_rights_of([root, reader], [boot_builtin("rx*")])
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: #reader uses directory "boot" with rights rw*,
         but the route holds less at the root's built-in:
           use      #reader "boot"                          rw*
           offer    root.cm "boot" from parent to #reader   none
           built-in "boot"                                  rx*   <- holds less
         The root will answer AccessDenied for this use. The root's built-in allows only rx*; ask the use for no more than that.
""",
        )

    def test_a_chain_that_only_narrows_warns_nothing(self) -> None:
        for name, scopes, builtins in [
            ("every hop as wide as the one before", directory_tree("r*"), None),
            ("a declaration wider than everything", directory_tree("r*", declared="rw*"), None),
            ("an expose that widens", directory_tree("r*", exposed={"rights": ["rx*"]}, declared="rx*"), None),
            ("a hop that states nothing", directory_tree(None), None),
            ("no offer rights, no expose rights", directory_tree(None, used="rw*"), None),
        ]:
            with self.subTest(name):
                self.assertEqual(check_rights_of(scopes, builtins), [])
        # The built-in: r* of rx*, and rx* of rx*.
        for used in ["r*", "rx*"]:
            root = scope_of("root", children=("reader",), offer=[directory_offer("boot", "parent", "#reader", "rx*")])
            reader = scope_of("reader", program=True, use=[directory_use("boot", "/boot", used)])
            self.assertEqual(check_rights_of([root, reader], [boot_builtin("rx*")]), [])

    def test_a_hop_that_states_no_rights_narrows_nothing(self) -> None:
        # The offer states none, so the expose is held against the use, and
        # r* of r* is fine; the declaration then allows rw*.
        self.assertEqual(check_rights_of(directory_tree(None, used="r*", exposed={"rights": ["r*"]})), [])
        # And one that states none does not hide the hop before it: the use
        # asks rx*, the offer states none, the expose states r*.
        (warning,) = check_rights_of(directory_tree(None, used="rx*", exposed={"rights": ["r*"]}))
        self.assertEqual(warning.walk.hops[-1].word, "expose")
        self.assertIn("ask the use for fewer.", cm.format_rights_warning(warning))

    def test_a_hop_is_held_against_the_hop_before_it_not_the_use(self) -> None:
        # reader asks r*. mid offers it rx* from its own `thin`, which root
        # offered with r*: r* would do for reader, but mid offered more than
        # it had, and it is root's offer that holds less than the offer in
        # front of it.
        root = scope_of(
            "root",
            children=("store", "mid"),
            offer=[directory_offer("data", "#store", "#mid", "r*", rename="thin")],
        )
        mid = scope_of(
            "mid",
            children=("reader",),
            offer=[directory_offer("thin", "parent", "#reader", "rx*", rename="wide")],
        )
        store = directory_tree(None)[1]
        reader = scope_of("reader", program=True, use=[directory_use("wide", "/wide", "r*")])
        (warning,) = check_rights_of([root, store, mid, reader])
        self.assertEqual(warning.instance.path, ("mid", "reader"))
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: #mid/#reader uses directory "wide" with rights r*,
         but the route holds less at an offer:
           use    #mid/#reader "wide"                            r*
           offer  #mid "thin" from parent to #reader as "wide"   rx*
           offer  root.cm "data" from #store to #mid as "thin"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the offer for fewer.
""",
        )

    def test_a_route_that_does_not_resolve_warns_nothing(self) -> None:
        # Each use asks rx* of offers that state rx*, and the hop where the
        # route stops would have held less (a declaration of rw*, a built-in
        # of r*), so a route that got there would be denied. None does.
        use = [directory_use("data", "/data", "rx*")]
        client = scope_of("client", program=True, use=use)
        declared = [{"directory": "data", "path": "/export/data", "rights": ["rw*"]}]
        hollow = scope_of(
            "store", capabilities=declared, expose=[{"directory": "data", "from": "self"}]
        )
        silent = scope_of("store", program=True)
        from_store = directory_offer("data", "#store", "#client", "rx*")
        from_parent = directory_offer("data", "parent", "#client", "rx*")
        above = scope_of("root", children=("client",), offer=[from_parent])
        cases = {
            "offered by nobody": ([scope_of("root", children=("client",)), client], None),
            "the child exposes nothing": (
                [scope_of("root", children=("client", "store"), offer=[from_store]), client, silent],
                None,
            ),
            "the provider has no program": (
                [scope_of("root", children=("client", "store"), offer=[from_store]), client, hollow],
                None,
            ),
            "offered from self, which has no program": (
                [
                    scope_of(
                        "root",
                        children=("client",),
                        capabilities=declared,
                        offer=[directory_offer("data", "self", "#client", "rx*")],
                    ),
                    client,
                ],
                None,
            ),
            "above the top, with no built-ins given": ([above, client], None),
            "above the top, built-ins that lack it": ([above, client], [boot_builtin()]),
            "above the top, a built-in of another kind": (
                [above, client],
                [cb.Builtin("data", cm.KIND_PROTOCOL, "data", None)],
            ),
        }
        for name, (scopes, builtins) in cases.items():
            with self.subTest(name):
                self.assertEqual(check_rights_of(scopes, builtins), [])

        # The same last route is denied, and warns, once the list has a
        # built-in of that name that allows less than the use asks.
        narrow = cb.Builtin("data", cm.KIND_DIRECTORY, "data", cm.RIGHTS["r*"])
        (warning,) = check_rights_of([above, client], [narrow])
        self.assertEqual(warning.walk.hops[-1].word, "built-in")

    def test_a_directory_use_never_reaches_a_protocol_nor_the_reverse(self) -> None:
        root = scope_of(
            "root",
            children=("store", "client"),
            offer=[
                directory_offer("status", "#store", "#client", "rx*"),
                directory_offer("data", "#store", "#client", "rx*", rename="as-protocol"),
            ],
        )
        # store exposes a protocol under the directory's name and a directory
        # under the protocol's. Each route would be denied at the declaration
        # (rw* does not contain rx*) if it got there.
        store = scope_of(
            "store",
            program=True,
            capabilities=[protocol("status"), {"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            expose=[{"protocol": "status", "from": "self"}, {"directory": "data", "from": "self"}],
        )
        client = scope_of(
            "client",
            program=True,
            use=[directory_use("status", "/status", "rx*"), protocol("as-protocol")],
        )
        self.assertEqual(check_rights_of([root, store, client]), [])

    def test_the_first_failure_on_the_walk_is_the_answer(self) -> None:
        # The offer is denied before the walk could find that the provider has
        # no program, which route() would answer NotFound for.
        root = scope_of(
            "root",
            children=("store", "client"),
            offer=[directory_offer("data", "#store", "#client", "r*")],
        )
        hollow = scope_of(
            "store",
            capabilities=[{"directory": "data", "path": "/export/data", "rights": ["rw*"]}],
            expose=[{"directory": "data", "from": "self"}],
        )
        client = scope_of("client", program=True, use=[directory_use("data", "/data", "rx*")])
        (warning,) = check_rights_of([root, hollow, client])
        self.assertEqual(warning.walk.hops[-1].word, "offer")

    def test_protocol_uses_are_not_walked(self) -> None:
        # The experiment of 2026-10-29: a type mismatch and no directory.
        self.assertEqual(check_rights_of(experiment_scopes(), [builtin("ours.process.Launcher")]), [])
        # A protocol use that states a type nothing could have, over offers
        # that would be denied if they were directories.
        root = scope_of("root", children=("server", "client"), offer=[offer("ours.example.Echo", "#server", "#client")])
        self.assertEqual(check_rights_of([root, server_scope(), client_scope(protocol("ours.example.Echo"))]), [])

    def test_the_walk_is_run_for_directory_uses_only(self) -> None:
        # No protocol hop states rights, so a walk of a protocol use could not
        # warn: the filter is seen in what is walked.
        scopes = directory_tree("r*", used="rx*")
        scopes[2]["manifest"]["use"].append(protocol("ours.process.Launcher"))
        walked = []
        original = cm.route_directory_use

        def spy(target, use, builtins, root_name):  # type: ignore[no-untyped-def]
            walked.append((target.path, use.source_name, use.kind))
            return original(target, use, builtins, root_name)

        cm.route_directory_use = spy
        try:
            self.assertEqual(len(check_rights_of(scopes)), 1)
        finally:
            cm.route_directory_use = original
        self.assertEqual(walked, [(("reader",), "data", cm.KIND_DIRECTORY)])

    def test_the_warnings_are_ordered_by_instance_path_then_use(self) -> None:
        root = scope_of(
            "root",
            children=("b", "a", "store"),
            offer=[
                directory_offer("data", "#store", "#a", "r*", rename="one"),
                directory_offer("data", "#store", "#a", "r*", rename="two"),
                directory_offer("data", "#store", "#b", "r*", rename="three"),
            ],
        )
        store = directory_tree(None)[1]
        a = scope_of("a", program=True, use=[directory_use("two", "/two", "rx*"), directory_use("one", "/one", "rx*")])
        b = scope_of("b", program=True, use=[directory_use("three", "/three", "rx*")])
        warnings = check_rights_of([root, b, a, store])
        self.assertEqual(
            [(w.instance.path, w.use.source_name) for w in warnings],
            [(("a",), "two"), (("a",), "one"), (("b",), "three")],
        )

    def test_a_manifest_under_two_parents_is_two_instances(self) -> None:
        # x.cm is both #a and #b: root offers it r* as `data` in one place
        # and rx* in the other.
        root = scope_of(
            "root",
            children=(("a", "x"), ("b", "x"), "store"),
            offer=[
                directory_offer("data", "#store", "#a", "r*"),
                directory_offer("data", "#store", "#b", "rx*"),
            ],
        )
        x = scope_of("x", program=True, use=[directory_use("data", "/data", "rx*")])
        store = directory_tree(None, declared="rx*")[1]
        warnings = check_rights_of([root, x, store])
        self.assertEqual([w.instance.path for w in warnings], [("a",)])

    def test_a_root_that_uses_a_directory_itself_is_walked(self) -> None:
        root = scope_of("root", use=[directory_use("boot", "/boot", "rw*")])
        (warning,) = check_rights_of([root], [boot_builtin("rx*")])
        self.assertEqual(
            cm.format_rights_warning(warning),
            """\
warning: //test:root.cm: root.cm uses directory "boot" with rights rw*,
         but the route holds less at the root's built-in:
           use      root.cm "boot"   rw*
           built-in "boot"           rx*   <- holds less
         The root will answer AccessDenied for this use. The root's built-in allows only rx*; ask the use for no more than that.
""",
        )

    def test_what_the_compiler_never_writes_is_walked_as_route_walks_it(self) -> None:
        # No manifest the compiler accepts states no rights on a directory
        # declaration or exposes from the parent, so the walk is given the
        # instances by hand, as TypeCheckTest does.
        use = cm.Use(cm.KIND_DIRECTORY, "d", "/d", "", cm.RIGHTS["rx*"])
        offer_d = cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_CHILD, "store"), "d", cm.Ref(cm.REF_CHILD, "client"), "d", "", cm.RIGHTS["rx*"])

        # root would answer it, if the walk turned back up at store: it
        # declares `d` too, and offers it to store.
        turned_back = cm.Offer(cm.KIND_DIRECTORY, cm.Ref(cm.REF_SELF), "d", cm.Ref(cm.REF_CHILD, "store"), "d", "", None)
        root_declares = cm.Capability(cm.KIND_DIRECTORY, "d", "", "/export/root", cm.RIGHTS["rw*"])

        def walk(
            expose_source: cm.Ref, capability: cm.Capability, program: bool = True
        ) -> Optional[cm.RightsWalk]:
            root_decl = cm.Decl(capabilities=[root_declares], offers=[offer_d, turned_back])
            root = cm.Instance("//test:root.cm", (), root_decl, True)
            store_decl = cm.Decl(
                capabilities=[capability],
                exposes=[cm.Expose(cm.KIND_DIRECTORY, expose_source, "d", "d")],
            )
            store = cm.Instance("//test:store.cm", ("store",), store_decl, program, root)
            client = cm.Instance("//test:client.cm", ("client",), cm.Decl(uses=[use]), True, root)
            root.children = {"store": store, "client": client}
            return cm.route_directory_use(client, use, None, "root.cm")

        declared = cm.Capability(cm.KIND_DIRECTORY, "d", "", "/export/d", cm.RIGHTS["r*"])
        self.assertTrue(walk(cm.Ref(cm.REF_SELF), declared).denied)
        # A declaration that states no rights narrows nothing.
        unbounded = cm.Capability(cm.KIND_DIRECTORY, "d", "", "/export/d", None)
        self.assertFalse(walk(cm.Ref(cm.REF_SELF), unbounded).denied)
        # An expose from the parent would turn the walk back up: not followed.
        self.assertIsNone(walk(cm.Ref(cm.REF_PARENT), declared))
        # A protocol capability of that name is not a directory.
        protocol_capability = cm.Capability(cm.KIND_PROTOCOL, "d", "")
        self.assertIsNone(walk(cm.Ref(cm.REF_SELF), protocol_capability))
        self.assertIsNone(walk(cm.Ref(cm.REF_SELF), unbounded, program=False))

    def test_rights_are_printed_as_a_manifest_writes_them(self) -> None:
        for rights, text in [
            (cm.RIGHTS["r*"], "r*"),
            (cm.RIGHTS["rx*"], "rx*"),
            (cm.RIGHTS["rw*"], "rw*"),
            (cm.RIGHTS["w*"], "w*"),
            (cm.RIGHTS["x*"], "x*"),
            (cm.RIGHTS["read_bytes"] | cm.RIGHTS["traverse"], "read_bytes,traverse"),
            (cm.RIGHTS["r*"] | cm.RIGHTS["execute"] | cm.RIGHTS["write_bytes"], "connect,enumerate,execute,get_attributes,read_bytes,traverse,write_bytes"),
            (0, "(empty)"),
            (None, "none"),
        ]:
            self.assertEqual(cm.rights_text(rights), text)

    def test_the_rights_of_a_warning_line_up(self) -> None:
        (warning,) = check_rights_of(directory_tree("rx*", used="rx*", declared="rw*", exposed={"rights": ["rx*"]}))
        rows = cm.format_rights_warning(warning).splitlines()[2:-1]
        self.assertEqual(len(rows), 4)
        columns = {row.rindex(" ", 0, row.index("*") - 1) for row in rows}
        self.assertEqual(len(columns), 1, rows)


class RightsWalkMainTest(unittest.TestCase):
    """The compiler's command line: a warning does not stop the build."""

    BOOT = {"builtins": [{"name": "boot", "kind": "directory", "rights": ["rx*"]}]}

    def test_a_denied_route_warns_and_the_build_goes_on(self) -> None:
        status, data, errors, deps, *_ = run_compile(denied_scopes(), self.BOOT, depfile=True)
        self.assertEqual(status, 0)
        self.assertEqual(errors, DENIED_WARNING)
        # The file is there, whole, and the depfile too.
        self.assertIsNotNone(data)
        self.assertEqual(Reader(data).total_size, len(data))
        self.assertIsNotNone(deps)

    def test_the_file_is_the_one_an_unwalked_build_writes(self) -> None:
        _, walked, *_ = run_compile(denied_scopes(), self.BOOT)
        status, unwalked, errors, *_ = run_compile(denied_scopes(), self.BOOT, ("--no-type-check",))
        self.assertEqual((status, errors), (0, ""))
        self.assertEqual(walked, unwalked)

    def test_no_type_check_turns_the_rights_walk_off_too(self) -> None:
        status, data, errors, *_ = run_compile(denied_scopes(), self.BOOT, ("--no-type-check",))
        self.assertEqual((status, errors), (0, ""))
        self.assertIsNotNone(data)

    def test_every_denied_use_has_a_block_of_its_own(self) -> None:
        root = scope_of(
            "root",
            children=("store", "reader"),
            offer=[
                directory_offer("data", "#store", "#reader", "r*", rename="one"),
                directory_offer("data", "#store", "#reader", "r*", rename="two"),
                directory_offer("data", "#store", "#reader", "rw*", rename="fine"),
            ],
        )
        store = directory_tree(None)[1]
        reader = scope_of(
            "reader",
            program=True,
            use=[
                directory_use("one", "/one", "rx*"),
                directory_use("two", "/two", "rx*"),
                directory_use("fine", "/fine", "r*"),
            ],
        )
        status, data, errors, *_ = run_compile([root, store, reader])
        self.assertEqual(status, 0)
        self.assertIsNotNone(data)
        self.assertEqual(errors.count("warning: //test:root.cm: "), 2)
        self.assertEqual(errors.count("The root will answer AccessDenied for this use."), 2)
        self.assertLess(errors.index('directory "one"'), errors.index('directory "two"'))
        self.assertNotIn('directory "fine"', errors)
        # Blocks are told apart by a blank line.
        self.assertIn("fewer.\n\nwarning: ", errors)

    def test_a_clean_tree_prints_nothing(self) -> None:
        status, data, errors, *_ = run_compile(directory_tree("r*"))
        self.assertEqual((status, errors), (0, ""))
        self.assertIsNotNone(data)

    def test_without_a_builtins_file_a_route_above_the_tree_is_not_walked(self) -> None:
        # The offer states nothing, so only the built-in can hold less than the
        # use, and nothing says what the built-in allows. (An offer that holds
        # less is in the tree and warns either way.)
        root = scope_of("root", children=("reader",), offer=[directory_offer("boot", "parent", "#reader")])
        reader = scope_of("reader", program=True, use=[directory_use("boot", "/boot", "rw*")])
        status, data, errors, *_ = run_compile([root, reader])
        self.assertEqual((status, errors), (0, ""))
        self.assertIsNotNone(data)
        status, _, errors, *_ = run_compile([root, reader], self.BOOT)
        self.assertEqual(status, 0)
        self.assertIn("holds less at the root's built-in", errors)
        # And the denied offer of dev's root.cm warns with no list at all.
        status, _, errors, *_ = run_compile(denied_scopes())
        self.assertEqual((status, errors), (0, DENIED_WARNING))

    def test_the_warnings_come_before_the_errors(self) -> None:
        # A tree with a denied directory route and a protocol mismatch: the
        # build fails for the mismatch, and says everything it knows.
        scopes = denied_scopes()
        scopes[0]["manifest"]["offer"].append(offer("ours.process.Launcher", "parent", "#routing_checks", "ours.resource.Irq"))
        scopes[1]["manifest"]["use"].append(protocol("ours.resource.Irq"))
        builtins = {"builtins": self.BOOT["builtins"] + [{"name": "ours.process.Launcher", "kind": "protocol"}]}
        status, data, errors, *_ = run_compile(scopes, builtins)
        self.assertEqual(status, 1)
        self.assertIsNone(data)
        self.assertTrue(errors.startswith("warning: "), errors)
        self.assertLess(errors.index("warning: "), errors.index("error: "))
        self.assertEqual(errors.count("warning: "), 1)
        self.assertEqual(errors.count("error: "), 1)

    def test_a_protocol_only_tree_prints_what_it_printed_before(self) -> None:
        status, data, errors, *_ = run_compile(experiment_scopes(), TypeCheckMainTest.LAUNCHER)
        self.assertEqual(status, 1)
        self.assertEqual(errors, EXPERIMENT_ERROR)


# The fixtures route_test.cpp routes through, as GN wrote their scopes. Named,
# not found by a glob: mismatch.cm is built to fail the check and must not be
# taken for a tree that resolves.
FIXTURES = "//system/lib/component/test/fixtures"
RESOLVING_TREES = ["realm", "top"]
MISMATCHED_TREES = ["mismatch"]
# The trees with directory uses. shelf.cm's are denied on purpose in part, which
# the build warns about and goes on.
DIRECTORY_TREES = ["realm", "top", "shelf"]
FIXTURE_NAMES = [
    "realm",
    "provider",
    "consumer",
    "hollow",
    "top",
    "user",
    "leaf",
    "mismatch",
    "shelf",
    "store",
    "reader",
    "bare",
]

# route_test.cpp's BUILTINS: boot (rx*), ours.process.Launcher and
# ours.resource.Debug.
ROUTE_TEST_BUILTINS = [
    boot_builtin("rx*"),
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


# What route() answers for every DIRECTORY use in each tree, as above: "<moniker>
# serves <name>" or "built-in <name>" for a route that resolves, the status,
# the place and the verb for one that route() denies ("." is the root), and None
# for one that does not resolve, whatever status and place route() reports. The
# walk reports an AccessDenied exactly as route() does and a NotFound as None.
EXPECTED_DIRECTORY_ROUTES: Dict[str, Dict[tuple, Optional[str]]] = {
    "realm": {
        # Route.ABuiltinDirectoryCarriesTheUsesRights
        (("consumer",), "boot"): "built-in boot",
        # Route.AnOffersSubdirGoesInFrontOfTheUses
        (("consumer",), "config"): "built-in boot",
    },
    "top": {
        # realm.cm's own, one level down.
        (("realm", "consumer"), "boot"): "built-in boot",
        (("realm", "consumer"), "config"): "built-in boot",
        # Route.SubdirsJoinFromTheSourceSide
        (("user",), "boot"): "built-in boot",
        (("user", "leaf"), "config"): "built-in boot",
        # Route.AnOfferNarrowerThanTheUseIsAccessDenied
        (("user",), "narrow"): "AccessDenied at .: Offer narrow",
        # Route.ABuiltinNarrowerThanTheUseIsAccessDenied
        (("user",), "writable"): "AccessDenied at the built-ins: Declare boot",
        # Route.RightsAreHeldAgainstTheHopBeforeNotTheUse
        (("user", "leaf"), "wide"): "AccessDenied at .: Offer thin",
    },
    "shelf": {
        # RouteShelf.ADirectoryUseEndsAtTheProviderWithTheDeclaredPath
        (("reader",), "notes"): "store serves data",
        # RouteShelf.AnOfferThatAsksMoreThanTheExposeStatesIsAccessDeniedAtTheChild
        (("reader",), "wide-offer"): "AccessDenied at store: Expose data",
        # RouteShelf.AnExposeThatAsksMoreThanTheDeclarationAllowsIsAccessDeniedAtTheProvider
        (("reader",), "wide-expose"): "AccessDenied at store: Declare data",
        # RouteShelf.AnExposeThatStatesNothingNarrowsNothing
        (("reader",), "plain"): "store serves data",
        # RouteShelf.ADeclarationNarrowerThanTheOfferIsAccessDeniedAtTheProvider
        (("reader",), "plain-wide"): "AccessDenied at store: Declare data",
        # RouteShelf.ADirectoryUseNeverReachesAProtocolCapability
        (("reader",), "wrong-kind"): None,
        # RouteShelf.AProviderWithoutAProgramIsNotFound
        (("reader",), "bare-data"): None,
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
        # A build directory can predate a fixture: it has some of the scopes
        # and not the new one. That is the same case as having none, a
        # `gn gen` to run, and not a failure of the walk.
        try:
            cls.scopes = {name: fixture_scope(build, name) for name in FIXTURE_NAMES}
        except AssertionError as error:
            raise unittest.SkipTest(f"{build} is older than the fixtures: {error}")

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

    def answer(self, walk: Optional[cm.RightsWalk]) -> Optional[str]:
        """The walk's result as the table above spells route()'s answer."""
        if walk is None:
            return None
        if walk.denied:
            hop = walk.hops[-1]
            where = "the built-ins"
            if hop.at is not None:
                where = "/".join(hop.at.path) or "."
            return f"AccessDenied at {where}: {hop.verb} {hop.name}"
        return self.moniker(cm.Resolved([], walk.end))

    def directory_routes(self, name: str, builtins: Optional[List[cb.Builtin]]) -> Dict[tuple, Optional[str]]:
        found: Dict[tuple, Optional[str]] = {}
        for instance in cm.instances(self.tree(name)):
            for use in instance.decl.uses:
                if use.kind != cm.KIND_DIRECTORY:
                    continue
                walk = cm.route_directory_use(instance, use, builtins, "root.cm")
                found[(instance.path, use.source_name)] = self.answer(walk)
        return found

    def test_every_directory_use_is_held_to_its_rights_as_route_does(self) -> None:
        for name in DIRECTORY_TREES:
            self.assertEqual(self.directory_routes(name, ROUTE_TEST_BUILTINS), EXPECTED_DIRECTORY_ROUTES[name], name)

    def test_the_expected_directory_routes_are_not_all_one_answer(self) -> None:
        # A walk that denied everything, or resolved everything, would agree
        # with a table of one answer.
        every = [answer for routes in EXPECTED_DIRECTORY_ROUTES.values() for answer in routes.values()]
        self.assertTrue(any(a and a.startswith("AccessDenied") for a in every))
        self.assertTrue(any(a and not a.startswith("AccessDenied") for a in every))
        self.assertTrue(any(a is None for a in every))

    def test_a_warning_is_exactly_an_access_denied(self) -> None:
        for name in DIRECTORY_TREES:
            scope = self.scopes[name]
            meta = [{"label": e["manifest"], "path": "meta/x"} for e in scope.get("children", [])]
            decl = cm.compile_manifest(scope, PROGRAM_NAMES if "program" in scope else [], meta)
            sources = {entry["label"]: entry for entry in self.scopes.values()}
            warned = {
                (w.instance.path, w.use.source_name)
                for w in cm.check_directory_rights(scope, decl, sources, ROUTE_TEST_BUILTINS)
            }
            denied = {
                key
                for key, answer in EXPECTED_DIRECTORY_ROUTES[name].items()
                if answer and answer.startswith("AccessDenied")
            }
            self.assertEqual(warned, denied, name)

    def test_without_the_builtins_a_denial_at_them_is_not_seen(self) -> None:
        # top.cm's `writable` is denied by the built-in boot's rx*; the other
        # two denials are in the tree. The real build of top.cm names no
        # built-ins, so it warns twice.
        expected = dict(EXPECTED_DIRECTORY_ROUTES["top"])
        expected[(("user",), "writable")] = None
        for key in [(("user",), "boot"), (("user", "leaf"), "config"), (("realm", "consumer"), "boot"), (("realm", "consumer"), "config")]:
            expected[key] = None
        self.assertEqual(self.directory_routes("top", None), expected)

    def test_the_denied_fixtures_build_with_warnings_and_without_failing(self) -> None:
        shelf = [self.scopes[name] for name in ["shelf", "store", "reader", "bare"]]
        status, data, errors, *_ = run_compile(shelf)
        self.assertEqual(status, 0)
        self.assertIsNotNone(data)
        warned = re.findall(r'uses directory "([^"]*)"', errors)
        self.assertEqual(sorted(warned), ["plain-wide", "wide-expose", "wide-offer"])
        # The file is the one a build without the walk writes.
        _, unwalked, errors, *_ = run_compile(shelf, extra=("--no-type-check",))
        self.assertEqual(errors, "")
        self.assertEqual(data, unwalked)

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
