import json
import pathlib
import sys
import tempfile
import unittest

from tools.annotation.cli import main


FIXTURES = pathlib.Path(__file__).parent / "fixtures"


class CliTest(unittest.TestCase):
    def command(self, compiler: pathlib.Path, config: pathlib.Path, output_root: pathlib.Path, manifest: pathlib.Path, source: pathlib.Path) -> list[str]:
        return [
            "--compiler",
            str(compiler),
            "--config",
            str(config),
            "--source",
            str(source),
            "--output-root",
            str(output_root),
            "--manifest",
            str(manifest),
        ]

    def fake_compiler(self, directory: pathlib.Path, output: str = "valid") -> pathlib.Path:
        compiler = directory / "fake-compiler.py"
        if output == "valid":
            body = (
                f"import pathlib, sys\n"
                f"sys.stdout.write(pathlib.Path({str(FIXTURES / 'ast_valid.json')!r}).read_text())\n"
            )
        else:
            body = "sys.stderr.write('compiler failed\\n'); raise SystemExit(2)\n"
        compiler.write_text(f"#!/usr/bin/env python3\n{body}", encoding="utf-8")
        compiler.chmod(0o755)
        return compiler

    def test_processes_sources_and_writes_outputs_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = pathlib.Path(directory_name)
            compiler = self.fake_compiler(directory)
            source = directory / "sample.cpp"
            source.write_text("source", encoding="utf-8")
            config = directory / "registrations.json"
            config.write_text(
                json.dumps(
                    {
                        "actions": [
                            {
                                "attribute": "ours::generate",
                                "declaration_kinds": ["function"],
                                "schema_version": 1,
                                "command": [sys.executable, str(FIXTURES / "external_action.py")],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output_root = directory / "out"
            manifest = directory / "manifest.json"

            status = main(self.command(compiler, config, output_root, manifest, source))

            self.assertEqual(status, 0)
            self.assertEqual(
                (output_root / "generated/action.txt").read_text(encoding="utf-8"),
                "action completed\n",
            )
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(manifest_data["outputs"], ["generated/action.txt"])
            self.assertIn("sample.cpp", manifest_data["dependencies"])

    def test_action_failure_prevents_output_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = pathlib.Path(directory_name)
            compiler = self.fake_compiler(directory)
            source = directory / "sample.cpp"
            source.write_text("source", encoding="utf-8")
            action = directory / "failed-action.py"
            action.write_text("import sys\nsys.stderr.write('action failed\\n')\nsys.exit(3)\n", encoding="utf-8")
            config = directory / "registrations.json"
            config.write_text(
                json.dumps(
                    {
                        "actions": [
                            {
                                "attribute": "ours::generate",
                                "declaration_kinds": ["function"],
                                "schema_version": 1,
                                "command": [sys.executable, str(action)],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output_root = directory / "out"
            manifest = directory / "manifest.json"

            status = main(self.command(compiler, config, output_root, manifest, source))

            self.assertEqual(status, 1)
            self.assertFalse(manifest.exists())
            self.assertFalse(output_root.exists())


if __name__ == "__main__":
    unittest.main()
