#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for component_builtins.py: the data file's rules, and the C++ it writes.

Run from anywhere: python3 system/test_component_builtins.py

The data file here is a fixture of its own, shaped like
system/service/init/builtins.json: the loader is tested without that file, so
the tests do not move when the root's list does.
"""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from typing import Any, List, Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import component_builtins as cb  # noqa: E402
import component_manifest as cm  # noqa: E402

CONTRACT = {
    "builtins": [
        {"name": "boot", "kind": "directory", "rights": ["rx*"]},
        {"name": "ours.process.Launcher", "kind": "protocol"},
        {"name": "ours.resource.Irq", "kind": "protocol", "type": "ours.resource.Irq"},
    ]
}


def write(directory: str, data: Any, name: str = "builtins.json") -> str:
    path = pathlib.Path(directory) / name
    path.write_text(data if isinstance(data, str) else json.dumps(data))
    return str(path)


class LoadTest(unittest.TestCase):
    def load(self, data: Any) -> List[cb.Builtin]:
        with tempfile.TemporaryDirectory() as directory:
            return cb.load(write(directory, data))

    def refuses(self, pattern: str, data: Any) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, data)
            with self.assertRaisesRegex(cb.BuiltinsError, pattern) as raised:
                cb.load(path)
            # Every refusal names the file, so that a build log says which.
            self.assertIn(path, str(raised.exception))

    def one(self, **clause: Any) -> Any:
        return {"builtins": [clause]}

    def test_the_contracts_example(self) -> None:
        self.assertEqual(
            self.load(CONTRACT),
            [
                cb.Builtin("boot", cm.KIND_DIRECTORY, "boot", cm.RIGHTS["rx*"]),
                cb.Builtin("ours.process.Launcher", cm.KIND_PROTOCOL, "ours.process.Launcher", None),
                cb.Builtin("ours.resource.Irq", cm.KIND_PROTOCOL, "ours.resource.Irq", None),
            ],
        )

    def test_the_order_of_the_file_is_kept(self) -> None:
        names = ["z", "a", "m"]
        data = {"builtins": [{"name": name, "kind": "protocol"} for name in names]}
        self.assertEqual([entry.name for entry in self.load(data)], names)

    def test_an_empty_list_is_a_list(self) -> None:
        self.assertEqual(self.load({"builtins": []}), [])

    def test_an_omitted_type_is_the_name(self) -> None:
        (entry,) = self.load(self.one(name="p.Q", kind="protocol"))
        self.assertEqual(entry.type, "p.Q")

    def test_a_stated_type_is_kept(self) -> None:
        (entry,) = self.load(self.one(name="p.Q", kind="protocol", type="p.Other"))
        self.assertEqual(entry.type, "p.Other")

    def test_a_directory_may_state_no_rights(self) -> None:
        (entry,) = self.load(self.one(name="d", kind="directory"))
        self.assertIsNone(entry.rights)

    def test_rights_use_the_words_of_a_manifest(self) -> None:
        (entry,) = self.load(self.one(name="d", kind="directory", rights=["read_bytes", "traverse"]))
        self.assertEqual(entry.rights, 0x02 | 0x80)
        (entry,) = self.load(self.one(name="d", kind="directory", rights=["r*"]))
        self.assertEqual(entry.rights, cm.RIGHTS["r*"])

    def test_rights_are_for_a_directory(self) -> None:
        self.refuses(
            r"builtins\[0\]\.rights: only a directory has `rights`",
            self.one(name="p", kind="protocol", rights=["r*"]),
        )

    def test_a_type_is_for_a_protocol(self) -> None:
        self.refuses(
            r"builtins\[0\]\.type: a directory has no `type`",
            self.one(name="d", kind="directory", type="t"),
        )

    def test_bad_rights_are_refused_as_a_manifest_refuses_them(self) -> None:
        for rights, error in [
            (["read"], "unknown right 'read'"),
            (["r*", "r*"], "duplicated in the rights clause"),
            ([], "non-empty list"),
        ]:
            self.refuses(error, self.one(name="d", kind="directory", rights=rights))

    def test_names_follow_the_rules_of_a_manifests(self) -> None:
        for bad, error in [
            ("", "must not be empty"),
            (".hidden", "is not a name"),
            ("a/b", "is not a name"),
            ("p" * (cm.MAX_NAME_LENGTH + 1), "longer than 100 bytes"),
            (7, "expected a string"),
        ]:
            self.refuses(error, self.one(name=bad, kind="protocol"))
            self.refuses(error, self.one(name="p", kind="protocol", type=bad))

    def test_a_kind_is_one_of_two_words(self) -> None:
        self.refuses(r"builtins\[0\]\.kind: \"service\" is not one of: directory, protocol", self.one(name="p", kind="service"))
        self.refuses(r"builtins\[0\]\.kind: 1 is not one of", self.one(name="p", kind=1))

    def test_name_and_kind_are_needed(self) -> None:
        self.refuses(r"builtins\[0\]: needs `name`", self.one(kind="protocol"))
        self.refuses(r"builtins\[0\]: needs `kind`", self.one(name="p"))

    def test_unknown_keys_are_refused(self) -> None:
        self.refuses(
            r"builtins\[0\]: unknown key 'version'; allowed here: name, kind, type, rights",
            self.one(name="p", kind="protocol", version=1),
        )
        self.refuses(r"unknown key 'extra'; allowed here: builtins", {"builtins": [], "extra": 1})

    def test_a_name_appears_once(self) -> None:
        clause = {"name": "p", "kind": "protocol"}
        self.refuses(
            r"builtins\[1\]\.name: 'p' is a duplicate built-in \(builtins\[0\]\)",
            {"builtins": [clause, clause]},
        )
        # Whatever the kind: the root finds a built-in by its name.
        self.refuses(
            "is a duplicate built-in",
            {"builtins": [clause, {"name": "p", "kind": "directory"}]},
        )

    def test_the_shape_of_the_file(self) -> None:
        self.refuses("expected a scope with a `builtins` list", [])
        self.refuses("needs a `builtins` list", {})
        self.refuses("needs a `builtins` list", {"builtins": {}})
        self.refuses(r"builtins\[0\]: expected a scope", {"builtins": ["boot"]})

    def test_a_file_that_is_not_json_or_not_there(self) -> None:
        self.refuses("is not JSON", "{not json")
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(cb.BuiltinsError, "cannot be read"):
                cb.load(str(pathlib.Path(directory) / "gone.json"))


class RenderTest(unittest.TestCase):
    def test_the_contracts_example(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            builtins = cb.load(write(directory, CONTRACT))
        self.assertEqual(
            cb.render(builtins, "system/service/init/builtins.json"),
            """\
// Generated from system/service/init/builtins.json by system/component_builtins.py. Do not edit.
//
// The root's built-in capabilities, in the order of that file. Include it
// where `BUILTINS` is to be defined, after <array>, <component/route.hpp>
// and <ustl/option.hpp>.
constexpr auto BUILTINS = std::array{
    component::Builtin{
        .kind = component::Kind::Directory,
        .name = "boot",
        .rights = ustl::Option<component::Rights>(component::Rights::RxStarDir),
        .type = "boot",
    },
    component::Builtin{
        .kind = component::Kind::Protocol,
        .name = "ours.process.Launcher",
        .rights = ustl::NONE,
        .type = "ours.process.Launcher",
    },
    component::Builtin{
        .kind = component::Kind::Protocol,
        .name = "ours.resource.Irq",
        .rights = ustl::NONE,
        .type = "ours.resource.Irq",
    },
};
""",
        )

    def render(self, *clauses: Any) -> str:
        with tempfile.TemporaryDirectory() as directory:
            path = write(directory, {"builtins": list(clauses)})
            return cb.render(cb.load(path), "b.json")

    def test_the_two_aliases_have_names_and_any_other_mask_is_a_cast(self) -> None:
        text = self.render(
            {"name": "a", "kind": "directory", "rights": ["r*"]},
            {"name": "b", "kind": "directory", "rights": ["rx*"]},
            {"name": "c", "kind": "directory", "rights": ["w*"]},
            {"name": "d", "kind": "directory", "rights": ["read_bytes", "traverse"]},
        )
        self.assertIn(
            ".rights = ustl::Option<component::Rights>(component::Rights::RStarDir),", text
        )
        self.assertIn(
            ".rights = ustl::Option<component::Rights>(component::Rights::RxStarDir),", text
        )
        self.assertIn(
            ".rights = ustl::Option<component::Rights>(static_cast<component::Rights>(0x1e5)),",
            text,
        )
        self.assertIn(
            ".rights = ustl::Option<component::Rights>(static_cast<component::Rights>(0x82)),",
            text,
        )

    def test_a_mask_equal_to_an_alias_is_that_alias(self) -> None:
        # r* spelled out word by word is the same mask, so the same name.
        text = self.render(
            {
                "name": "a",
                "kind": "directory",
                "rights": ["connect", "enumerate", "traverse", "read_bytes", "get_attributes"],
            }
        )
        self.assertIn("component::Rights::RStarDir", text)

    def test_the_type_is_always_written(self) -> None:
        text = self.render(
            {"name": "p", "kind": "protocol"},
            {"name": "q", "kind": "protocol", "type": "q.Other"},
        )
        self.assertIn('.name = "p",\n        .rights = ustl::NONE,\n        .type = "p",', text)
        self.assertIn('.name = "q",\n        .rights = ustl::NONE,\n        .type = "q.Other",', text)

    def test_designated_initialisers_follow_the_declaration_order(self) -> None:
        # component::Builtin is {kind, name, rights, type}: a designated
        # initialiser out of order does not compile.
        text = self.render({"name": "d", "kind": "directory", "rights": ["r*"]})
        order = [text.index(f".{field} =") for field in ["kind", "name", "rights", "type"]]
        self.assertEqual(order, sorted(order))

    def test_an_empty_list_is_an_empty_array_of_builtins(self) -> None:
        # std::array{} of nothing has no type to deduce.
        self.assertIn(
            "constexpr auto BUILTINS = std::array<component::Builtin, 0>{};\n", self.render()
        )

    def test_no_include_of_its_own(self) -> None:
        # It sits inside a namespace in the file that includes it, where a
        # standard header would be declared in the wrong place.
        self.assertNotIn("#include", self.render({"name": "p", "kind": "protocol"}))


class MainTest(unittest.TestCase):
    def run_main(self, data: Any) -> tuple:
        with tempfile.TemporaryDirectory() as directory:
            source = write(directory, data)
            output = pathlib.Path(directory) / "gen" / "builtins.inc"
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                status = cb.main(["--input", source, "--output", str(output)])
            text = output.read_text() if output.exists() else None
            return status, text, errors.getvalue(), source

    def test_writes_the_include_file(self) -> None:
        status, text, errors, source = self.run_main(CONTRACT)
        self.assertEqual((status, errors), (0, ""))
        # The comment at its top says which file it came from.
        self.assertTrue(text.startswith(f"// Generated from {source} by system/component_builtins.py."))
        self.assertIn('.name = "ours.resource.Irq"', text)

    def test_a_refusal_writes_nothing(self) -> None:
        status, text, errors, _ = self.run_main({"builtins": [{"name": "p"}]})
        self.assertEqual(status, 1)
        self.assertIsNone(text)
        self.assertTrue(errors.startswith("component_builtins: "), errors)
        self.assertIn("builtins[0]: needs `kind`", errors)


if __name__ == "__main__":
    unittest.main()
