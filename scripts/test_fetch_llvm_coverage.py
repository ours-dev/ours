import contextlib
import io
import os
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import fetch_llvm_coverage as fetch

COMMIT = "a" * 40
OTHER_COMMIT = "b" * 40
HOST = fetch.Host("Test-Host", "testos", "libclang_rt.profile_test.a", True)


def script(text):
    return ("#!/bin/sh\n" + text + "\n").encode()


def fake_compiler(base, version="23.1.0-rc3", commit=COMMIT, banner=None):
    """A directory shaped like prebuilt/clang/llvm-N whose bin/clang answers the two
    questions the script asks it."""
    directory = base / "llvm-9"
    (directory / "bin").mkdir(parents=True, exist_ok=True)
    banner = banner or f"clang version {version} (https://github.com/llvm/llvm-project {commit})"
    clang = directory / "bin" / "clang"
    clang.write_bytes(script(
        f'if [ "$1" = "--print-resource-dir" ]; then echo "{directory}/lib/clang/23"; '
        f'else echo "{banner}"; fi'))
    clang.chmod(0o755)
    return directory


def fake_archive(releases, version="23.1.0-rc3", commit=COMMIT, tools_version=None):
    """Writes releases/llvmorg-<version>/LLVM-<version>-Test-Host.tar.xz laid out as
    upstream's is: a top directory, bin/clang a link to bin/clang-23, and
    bin/llvm-profdata a link to another tool."""
    tools_version = tools_version or version
    top = f"LLVM-{version}-Test-Host"
    path = releases / f"llvmorg-{version}" / f"{top}.tar.xz"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "w:xz") as archive:
        def add_file(name, data, mode=0o755):
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size = len(data)
            info.mode = mode
            archive.addfile(info, io.BytesIO(data))

        def add_link(name, target):
            info = tarfile.TarInfo(f"{top}/{name}")
            info.type = tarfile.SYMTYPE
            info.linkname = target
            archive.addfile(info)

        add_file("bin/clang-23", b"\0junk\0" + commit.encode() + b"\0junk\0")
        add_link("bin/clang", "clang-23")
        add_file("bin/llvm-cov", script(f'echo "LLVM version {tools_version}"'))
        add_file("bin/llvm-profdata-real", script(f'echo "LLVM version {tools_version}"'))
        add_link("bin/llvm-profdata", "llvm-profdata-real")
        add_file(f"lib/clang/{version.split('.')[0]}/lib/testos/libclang_rt.profile_test.a",
                 b"!<arch>\n", 0o644)
    return path


class FetchLlvmCoverage(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)
        self.releases = self.base / "releases"
        self.url = self.releases.as_uri()
        patcher = mock.patch.object(fetch, "detect_host", return_value=HOST)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_main(self, directory, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = fetch.main(["--clang-dir", str(directory), "--base-url", self.url, *extra])
        return code, out.getvalue()

    def leftovers(self, directory):
        return [p.name for p in directory.iterdir() if p.name.startswith(".fetch")]

    def test_reads_version_and_commit_from_the_compiler(self):
        compiler = fetch.probe(fake_compiler(self.base))
        self.assertEqual(compiler.version, "23.1.0-rc3")
        self.assertEqual(compiler.commit, COMMIT)

    def test_a_distribution_compiler_has_no_commit_and_cannot_be_fetched_for(self):
        directory = fake_compiler(self.base, banner="Debian clang version 22.1.0 (++2026-1)")
        compiler = fetch.probe(directory)
        self.assertEqual(compiler.version, "22.1.0")
        self.assertIsNone(compiler.commit)
        code, output = self.run_main(directory)
        self.assertEqual(code, 1)
        self.assertIn("without an upstream release commit", output)

    def test_installs_tools_and_runtime_following_links(self):
        directory = fake_compiler(self.base)
        fake_archive(self.releases)
        code, output = self.run_main(directory)
        self.assertEqual(code, 0, output)
        for tool in fetch.TOOLS:
            self.assertTrue(os.access(directory / "bin" / tool, os.X_OK), tool)
        self.assertTrue((directory / "lib/clang/23/lib/testos/libclang_rt.profile_test.a").is_file())
        self.assertEqual(self.leftovers(directory), [])
        self.assertEqual(fetch.problems(fetch.probe(directory), HOST), [])

    def test_a_second_run_downloads_nothing(self):
        directory = fake_compiler(self.base)
        fake_archive(self.releases)
        self.assertEqual(self.run_main(directory)[0], 0)
        shutil.rmtree(self.releases)
        code, output = self.run_main(directory)
        self.assertEqual(code, 0, output)
        self.assertIn("Already available", output)

    def test_an_archive_built_from_another_commit_changes_nothing(self):
        directory = fake_compiler(self.base)
        fake_archive(self.releases, commit=OTHER_COMMIT)
        code, output = self.run_main(directory)
        self.assertEqual(code, 1)
        self.assertIn("was not built from commit", output)
        self.assertFalse((directory / "bin" / "llvm-cov").exists())
        self.assertFalse((directory / "lib").exists())
        self.assertEqual(self.leftovers(directory), [])

    def test_tools_of_another_release_than_the_archive_claims_change_nothing(self):
        directory = fake_compiler(self.base)
        fake_archive(self.releases, tools_version="23.1.0")
        code, output = self.run_main(directory)
        self.assertEqual(code, 1)
        self.assertFalse((directory / "bin" / "llvm-cov").exists())

    def test_a_new_compiler_makes_the_next_run_fetch_again(self):
        directory = fake_compiler(self.base)
        fake_archive(self.releases)
        self.assertEqual(self.run_main(directory)[0], 0)
        fake_compiler(self.base, version="24.1.0", commit=OTHER_COMMIT)
        self.assertIn("reports LLVM 23.1.0-rc3",
                      "\n".join(fetch.problems(fetch.probe(directory), HOST)))
        fake_archive(self.releases, version="24.1.0", commit=OTHER_COMMIT)
        code, output = self.run_main(directory)
        self.assertEqual(code, 0, output)
        self.assertEqual(fetch.reported_release(directory / "bin" / "llvm-cov"), "24.1.0")

    def test_a_missing_archive_is_reported_and_changes_nothing(self):
        directory = fake_compiler(self.base)
        code, output = self.run_main(directory)
        self.assertEqual(code, 1)
        self.assertIn("cannot download", output)
        self.assertEqual(self.leftovers(directory), [])

    def test_contains_finds_a_needle_across_two_reads(self):
        data = b"x" * (fetch.CHUNK - 10) + b"NEEDLE" + b"y" * 100
        self.assertTrue(fetch.contains(io.BytesIO(data), b"NEEDLE"))
        self.assertFalse(fetch.contains(io.BytesIO(data), b"NEEDLES"))

    def test_the_highest_numbered_compiler_that_runs_is_chosen(self):
        root = self.base / "clang"
        for number in (22, 23):
            (root / f"llvm-{number}" / "bin").mkdir(parents=True)
        (root / "llvm-22" / "bin" / "clang").symlink_to(self.base / "nowhere")
        runs = script('echo "clang version 23.1.0-rc3 (https://github.com/llvm/llvm-project '
                      + COMMIT + ')"')
        (root / "llvm-23" / "bin" / "clang").write_bytes(runs)
        (root / "llvm-23" / "bin" / "clang").chmod(0o755)
        with mock.patch.object(fetch, "CLANG_ROOT", root):
            self.assertEqual(fetch.find_compiler(None).directory, root / "llvm-23")


if __name__ == "__main__":
    unittest.main()
