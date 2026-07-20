import json
import pathlib
import tempfile
import unittest

from tools.annotation.discovery import (
    DiscoveryError,
    discover_from_ast,
    run_clang_ast_dump,
)
from tools.annotation.model import ActionSpec
from tools.annotation.registry import Registry


FIXTURES = pathlib.Path(__file__).parent / "fixtures"


def make_registry() -> Registry:
    registry = Registry()
    registry.register(ActionSpec("ours::generate", ("function",), 1, command=("generate",)))
    return registry


class DiscoveryTest(unittest.TestCase):
    def load_fixture(self, name: str) -> dict[str, object]:
        return json.loads((FIXTURES / name).read_text(encoding="utf-8"))

    def test_discovers_registered_attribute_on_function(self):
        records = discover_from_ast(
            self.load_fixture("ast_valid.json"),
            pathlib.PurePosixPath("src/sample.cpp"),
            make_registry(),
        )

        self.assertEqual([record.line for record in records], [3, 8])
        self.assertEqual(records[0].canonical_name, "ours::generate")
        self.assertEqual(records[0].qualified_name, "sample::parse")
        self.assertEqual(records[0].arguments["format"], "text")

    def test_ignores_attributes_outside_registered_namespaces(self):
        ast = {
            "kind": "TranslationUnitDecl",
            "inner": [
                {
                    "kind": "FunctionDecl",
                    "name": "ordinary",
                    "qualifiedName": "ordinary",
                    "loc": {"line": 4, "col": 1},
                    "inner": [{"kind": "CXX11Attribute", "name": "gnu::cold"}],
                }
            ],
        }

        self.assertEqual(discover_from_ast(ast, pathlib.PurePosixPath("x.cpp"), make_registry()), [])

    def test_rejects_unknown_attribute_in_registered_namespace(self):
        ast = {
            "kind": "TranslationUnitDecl",
            "inner": [
                {
                    "kind": "FunctionDecl",
                    "name": "unknown",
                    "qualifiedName": "unknown",
                    "loc": {"line": 7, "col": 2},
                    "inner": [{"kind": "CXX11Attribute", "name": "ours::unknown"}],
                }
            ],
        }

        with self.assertRaisesRegex(DiscoveryError, r"x.cpp:7:2"):
            discover_from_ast(ast, pathlib.PurePosixPath("x.cpp"), make_registry())

    def test_rejects_unsupported_declaration_kind(self):
        ast = {
            "kind": "TranslationUnitDecl",
            "inner": [
                {
                    "kind": "NamespaceDecl",
                    "name": "sample",
                    "qualifiedName": "sample",
                    "loc": {"line": 2, "col": 1},
                    "inner": [{"kind": "CXX11Attribute", "name": "ours::generate"}],
                }
            ],
        }

        with self.assertRaises(DiscoveryError):
            discover_from_ast(ast, pathlib.PurePosixPath("x.cpp"), make_registry())

    def test_rejects_registered_attribute_without_declaration_identity(self):
        with self.assertRaises(DiscoveryError):
            discover_from_ast(
                self.load_fixture("ast_malformed.json"),
                pathlib.PurePosixPath("x.cpp"),
                make_registry(),
            )

    def test_runs_compiler_and_parses_ast_json(self):
        ast_text = json.dumps(self.load_fixture("ast_valid.json"))
        with tempfile.TemporaryDirectory() as directory:
            compiler = pathlib.Path(directory) / "fake-clang"
            compiler.write_text(
                "#!/bin/sh\n"
                f"printf '%s' '{ast_text}'\n",
                encoding="utf-8",
            )
            compiler.chmod(0o755)

            ast = run_clang_ast_dump(str(compiler), [], pathlib.Path("sample.cpp"))

        self.assertEqual(ast["kind"], "TranslationUnitDecl")

    def test_rejects_invalid_ast_json_from_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            compiler = pathlib.Path(directory) / "fake-clang"
            compiler.write_text("#!/bin/sh\nprintf '%s' invalid\n", encoding="utf-8")
            compiler.chmod(0o755)

            with self.assertRaisesRegex(DiscoveryError, "invalid Clang AST JSON"):
                run_clang_ast_dump(str(compiler), [], pathlib.Path("sample.cpp"))


if __name__ == "__main__":
    unittest.main()
