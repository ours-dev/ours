"""Tests of scripts/ci.py on throwaway git repositories and build directories."""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import ci
import gitdiff


def git(repo: Path, *args: str) -> str:
    env = {**gitdiff.git_env(), "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True,
                          text=True, env=env).stdout.strip()


class Repo(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary.name)
        git(self.repo, "init", "-q", "-b", "main")
        self.commit({"a.cpp": "int a;\n", "README.md": "# a\n"})
        self.first = git(self.repo, "rev-parse", "HEAD")

    def tearDown(self):
        self.temporary.cleanup()

    def commit(self, files):
        for name, text in files.items():
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "x")


class BaseTest(Repo):
    def test_changed_paths_from_the_merge_base(self):
        git(self.repo, "checkout", "-q", "-b", "topic")
        self.commit({"b.gn": "group(\"b\") {}\n", "a.cpp": "int a = 1;\n"})
        git(self.repo, "checkout", "-q", "main")
        self.commit({"main-only.md": "# m\n"})
        git(self.repo, "checkout", "-q", "topic")
        start = ci.merge_base("main", cwd=self.repo)
        self.assertEqual(start, self.first)
        self.assertEqual(ci.changed_paths(start, cwd=self.repo), ["a.cpp", "b.gn"])

    def test_a_first_push_compares_with_main(self):
        git(self.repo, "checkout", "-q", "-b", "topic")
        self.commit({"c.cpp": "int c;\n"})
        with mock.patch.object(ci, "FIRST_PUSH_BASE", "main"):
            self.assertEqual(ci.merge_base(ci.NO_COMMIT, cwd=self.repo), self.first)
            self.assertEqual(ci.merge_base("", cwd=self.repo), self.first)

    def test_an_unknown_base_is_an_error(self):
        with self.assertRaises(ci.CiError):
            ci.merge_base("no-such-branch", cwd=self.repo)

    def test_the_checkers_diff_against_the_base_they_are_given(self):
        self.commit({"a.cpp": "int a;\nint b;\n"})
        with mock.patch.dict(os.environ, {gitdiff.BASE_VARIABLE: self.first}):
            diff = gitdiff.repo_diff(self.repo)
        self.assertEqual(gitdiff.added_lines(diff, "a.cpp"), {2})
        with mock.patch.dict(os.environ, {gitdiff.BASE_VARIABLE: ""}):
            self.assertEqual(gitdiff.repo_diff(self.repo), "")


class CheckTest(Repo):
    def test_each_checker_gets_its_files_and_the_base(self):
        paths = ["w.py", "x.hpp", "y.gni", "z.md"]
        calls = []

        def run_step(name, command, env=None):
            calls.append((name, command, (env or {}).get(gitdiff.BASE_VARIABLE)))
            return True

        with mock.patch.object(ci, "ROOT", self.repo), \
                mock.patch.object(ci, "merge_base", lambda base: self.first), \
                mock.patch.object(ci, "changed_paths", lambda base: paths), \
                mock.patch.object(ci, "run_step", run_step), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(ci.check(self.first))
        by_name = {name: (command, base) for name, command, base in calls}
        self.assertEqual(by_name["format-cpp"][0][-1], "x.hpp")
        self.assertEqual(by_name["format-gn"][0][-1], "y.gni")
        self.assertEqual(by_name["lint-docs"][0][-2:], ["--changed", "z.md"])
        self.assertEqual(by_name["format-cpp"][1], self.first)
        self.assertIn("script-tests", by_name)


class ArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.out = self.base / "out"
        self.dest = self.base / "artifacts"
        self.out.mkdir()
        (self.out / "product_info.json").write_text(json.dumps(
            [{"kernel_images": [{"output_name": "dev"}, {"output_name": "dev.dbg_1"}]}]))
        for name in ("dev", "dev.dbg_1"):
            (self.out / f"kernel.{name}.obi").write_bytes(b"obi " + name.encode())
            (self.out / f"kernel.{name}.debug.json").write_text(json.dumps(
                [{"debug": f"obj/{name}/vmours", "environment": "kernel"},
                 {"image": f"kernel.{name}", "obi": f"kernel.{name}.obi"}]))
            elf = self.out / "obj" / name / "vmours"
            elf.parent.mkdir(parents=True)
            elf.write_bytes(b"\x7fELF")
        self.logs = self.base / "logs"
        self.logs.mkdir()
        (self.logs / "dbg.log").write_text("boot\n")
        (self.logs / "esp.img").write_bytes(b"big")
        patch = mock.patch.object(ci, "log_directory", lambda prefix, out: self.logs)
        patch.start()
        self.addCleanup(patch.stop)

    def tearDown(self):
        self.temporary.cleanup()

    def test_collects_what_gn_names_and_the_logs(self):
        self.assertEqual(ci.collect(self.out, self.dest), [])
        files = sorted(p.relative_to(self.dest).as_posix() for p in self.dest.rglob("*") if p.is_file())
        self.assertEqual(files, ["SHA256SUMS", "debug/obj/dev.dbg_1/vmours", "debug/obj/dev/vmours",
                                 "kernel.dev.dbg_1.debug.json", "kernel.dev.dbg_1.obi",
                                 "kernel.dev.debug.json", "kernel.dev.obi", "logs/dbg.log"])
        sums = (self.dest / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(sums), 7)
        self.assertTrue(any(line.endswith("  kernel.dev.obi") for line in sums))

    def test_a_failed_build_still_saves_the_logs(self):
        (self.out / "kernel.dev.obi").unlink()
        missing = ci.collect(self.out, self.dest)
        self.assertEqual(missing, [str(self.out / "kernel.dev.obi")])
        self.assertTrue((self.dest / "logs/dbg.log").is_file())


if __name__ == "__main__":
    unittest.main()
