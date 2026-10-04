#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for component_manifest.py: each rule it checks, and what it writes.

Run from anywhere: python3 system/test_component_manifest.py

The bytes it writes are also decoded by the C++ decoder's own test
(//system/lib/component/test), from manifests the GN template built, so the
two halves of the format cannot drift apart unnoticed.
"""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import struct
import sys
import tempfile
import unittest
from typing import Any, Dict, List

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

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

    def test_rights_aliases_match_fuchsia_io(self) -> None:
        # R_STAR_DIR, RX_STAR_DIR and W_STAR_DIR in rights-abilities.fidl.
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
        # Across kinds too, as cml keys the check on the name alone.
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

    def test_manifest_name(self) -> None:
        self.refuses("ends in .cm", {"label": "//test:test"}, [], [])
        self.refuses("not a name", {"label": "//test:.hidden.cm"}, [], [])


class EncodeTest(unittest.TestCase):
    def compiled(self) -> bytes:
        manifest = {
            "program": {"args": ["--serve", "echo"]},
            "capabilities": [{"protocol": "fuchsia.example.Echo"}],
            "use": [
                {"directory": "boot", "path": "/boot", "rights": ["rx*"]},
                {"protocol": "fuchsia.process.Launcher"},
            ],
            "offer": [{"protocol": "fuchsia.example.Echo", "from": "self", "to": "#child"}],
            "expose": [{"protocol": "fuchsia.example.Echo", "from": "self"}],
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
            (cm.CAPABILITY, 16),
            (cm.USE, 48),
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
        texts = [reader.text(strings[i], strings[i + 1]) for i in range(0, 6, 2)]
        self.assertEqual(texts, ["boot", "/boot", ""])
        self.assertEqual(rights, cm.RIGHTS["rx*"])
        kind, flags, *_, rights = protocol
        self.assertEqual((kind, flags, rights), (cm.KIND_PROTOCOL, 0, 0))

        offer = reader.tables["offer"][0]
        self.assertEqual(offer[2:4], (cm.REF_SELF, 0))
        self.assertEqual(offer[6:8], (cm.REF_CHILD, 0))

        expose = reader.tables["expose"][0]
        self.assertEqual(expose[2:4], (cm.REF_SELF, 0))
        self.assertEqual(expose[6:8], (cm.REF_PARENT, 0))

    def test_strings_are_stored_once(self) -> None:
        reader = Reader(self.compiled())
        self.assertEqual(reader.strings.count(b"fuchsia.example.Echo"), 1)

    def test_every_u64_is_aligned(self) -> None:
        # The rights words fall on multiples of 8 in each record, and every
        # record size is one, so a reader that maps the file sees them
        # naturally aligned.
        self.assertEqual(struct.calcsize("<IIIIIIIIII"), 40)
        self.assertEqual(struct.calcsize("<IIIIIIIIIIII"), 48)
        for layout in [cm.HEADER, cm.PROGRAM, cm.ARG, cm.CHILD, cm.CAPABILITY, cm.USE, cm.OFFER, cm.EXPOSE]:
            self.assertEqual(layout.size % 8, 0)


class MainTest(unittest.TestCase):
    def run_main(self, scope: Dict[str, Any], names: List[Any], children: List[Any]) -> tuple:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = {}
            for key, value in [("source", scope), ("program", names), ("children", children)]:
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
                        "--output", str(output),
                    ]
                )
            data = output.read_bytes() if output.exists() else None
        return status, data, errors.getvalue()

    def test_writes_the_manifest(self) -> None:
        status, data, errors = self.run_main(source(), [], [])
        self.assertEqual((status, errors), (0, ""))
        self.assertEqual(Reader(data).total_size, len(data))

    def test_refusal_writes_nothing(self) -> None:
        status, data, errors = self.run_main(source({"bogus": 1}), [], [])
        self.assertEqual(status, 1)
        self.assertIsNone(data)
        self.assertIn("unknown key 'bogus'", errors)


if __name__ == "__main__":
    unittest.main()
