"""Offline tests of scripts/fetch_prebuilt.py: archives are built here, put in
the downloads directory under their hash or served by a fake connection, so
nothing touches the network."""
import contextlib
import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import fetch_prebuilt as fetch


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tar_xz(entries) -> bytes:
    """entries: (name, kind, payload) with kind 'file', 'exe', 'dir', 'symlink' or 'hardlink'."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:xz") as archive:
        for name, kind, payload in entries:
            info = tarfile.TarInfo(name)
            if kind in ("file", "exe"):
                info.size = len(payload)
                info.mode = 0o755 if kind == "exe" else 0o644
                archive.addfile(info, io.BytesIO(payload))
                continue
            if kind == "dir":
                info.type = tarfile.DIRTYPE
            elif kind == "symlink":
                info.type = tarfile.SYMTYPE
                info.linkname = payload
            else:
                info.type = tarfile.LNKTYPE
                info.linkname = payload
            archive.addfile(info)
    return buffer.getvalue()


def zip_bytes(name: str, payload: bytes, mode: int) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        info = zipfile.ZipInfo(name)
        info.external_attr = mode << 16
        archive.writestr(info, payload)
    return buffer.getvalue()


LLVM_LIKE = tar_xz([
    ("top/bin/clang-23", "exe", b"#!/bin/sh\necho 'clang version 23.1.0-rc3'\n"),
    ("top/bin/clang", "symlink", "clang-23"),
    ("top/bin/clang++", "hardlink", "top/bin/clang-23"),
    ("top/bin/unwanted", "exe", b"x"),
    ("top/lib/x/a.txt", "file", b"a"),
])


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.prebuilt = self.base / "prebuilt"
        self.downloads = self.base / "downloads"
        self.downloads.mkdir()
        self.manifest = self.base / "prebuilt.json"

    def tearDown(self):
        self.temporary.cleanup()

    def offer(self, data: bytes) -> str:
        """Puts an archive where the script looks before downloading."""
        digest = sha256(data)
        (self.downloads / digest).write_bytes(data)
        return digest

    def write_manifest(self, tools) -> None:
        self.manifest.write_text(json.dumps({"host": fetch.host(), "tools": tools}))

    def run_script(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = fetch.main(["--manifest", str(self.manifest), "--dest", str(self.prebuilt),
                               "--downloads", str(self.downloads), *args])
        return code, output.getvalue()

    def llvm_entry(self, **source_overrides):
        source = {"url": "https://example.invalid/llvm.tar.xz", "sha256": self.offer(LLVM_LIKE),
                  "strip": 1, "keep": ["bin/clang", "bin/clang++", "bin/clang-23", "lib/x"],
                  "rename": {"lib/x": "lib/y"}}
        source.update(source_overrides)
        return {"dest": "clang/llvm-23", "sources": [source],
                "files": {".versions/clang.cipd_version": "{}\n"},
                "check": {"command": ["bin/clang", "--version"],
                          "expect": "clang version 23.1.0-rc3"}}


class InstallTest(Fixture):
    def test_installs_kept_members_links_and_files(self):
        self.write_manifest({"clang": self.llvm_entry()})
        code, output = self.run_script()
        self.assertEqual(code, 0, output)
        tree = self.prebuilt / "clang/llvm-23"
        self.assertEqual(os.readlink(tree / "bin/clang"), "clang-23")
        self.assertEqual((tree / "bin/clang++").stat().st_ino, (tree / "bin/clang-23").stat().st_ino)
        self.assertEqual(oct((tree / "bin/clang-23").stat().st_mode & 0o777), oct(0o755))
        self.assertFalse((tree / "bin/unwanted").exists())
        self.assertEqual((tree / "lib/y/a.txt").read_text(), "a")
        self.assertTrue((tree / ".versions/clang.cipd_version").is_file())
        self.assertTrue((tree / fetch.STAMP).is_file())

    def test_a_current_tree_is_left_alone(self):
        self.write_manifest({"clang": self.llvm_entry()})
        self.run_script()
        marker = self.prebuilt / "clang/llvm-23/marker"
        marker.write_text("kept")
        code, output = self.run_script()
        self.assertEqual(code, 0, output)
        self.assertIn("clang: current", output)
        self.assertTrue(marker.exists())

    def test_a_tree_from_another_pin_is_replaced(self):
        self.write_manifest({"clang": self.llvm_entry()})
        self.run_script()
        entry = self.llvm_entry()
        entry["files"][".versions/clang.cipd_version"] = '{"instance_id": "new"}\n'
        self.write_manifest({"clang": entry})
        code, output = self.run_script()
        self.assertEqual(code, 0, output)
        self.assertIn("new", (self.prebuilt / "clang/llvm-23/.versions/clang.cipd_version").read_text())

    def test_a_tree_not_made_here_needs_force(self):
        foreign = self.prebuilt / "clang/llvm-23"
        foreign.mkdir(parents=True)
        (foreign / "hand-built").write_text("mine")
        self.write_manifest({"clang": self.llvm_entry()})
        code, output = self.run_script()
        self.assertEqual(code, 0, output)
        self.assertIn("left alone", output)
        self.assertTrue((foreign / "hand-built").exists())
        code, output = self.run_script("--force")
        self.assertEqual(code, 0, output)
        self.assertFalse((foreign / "hand-built").exists())

    def test_check_reports_without_changing_anything(self):
        self.write_manifest({"clang": self.llvm_entry()})
        code, output = self.run_script("--check")
        self.assertEqual(code, 1)
        self.assertIn("clang: missing", output)
        self.assertFalse((self.prebuilt / "clang").exists())

    def test_zip_keeps_the_executable_bit(self):
        data = zip_bytes("gn", b"#!/bin/sh\necho 2530\n", 0o744)
        self.write_manifest({"gn": {"dest": "gn", "sources": [
            {"url": "https://example.invalid/gn.zip", "sha256": self.offer(data),
             "format": "zip", "keep": ["gn"]}],
            "check": {"command": ["gn", "--version"], "expect": "2530"}}})
        code, output = self.run_script()
        self.assertEqual(code, 0, output)
        self.assertEqual(oct((self.prebuilt / "gn/gn").stat().st_mode & 0o777), oct(0o755))


class RefusalTest(Fixture):
    def assert_refused(self, entry, message):
        self.write_manifest({"clang": entry})
        code, output = self.run_script()
        self.assertEqual(code, 1, output)
        self.assertIn(message, output)
        self.assertFalse((self.prebuilt / "clang/llvm-23").exists())
        self.assertEqual([p.name for p in (self.prebuilt / "clang").glob(".fetch-*")], [])

    def test_a_keep_prefix_that_matches_nothing(self):
        self.assert_refused(self.llvm_entry(keep=["bin/clang", "bin/clang-23", "bin/lld"]),
                            "nothing in the archive under bin/lld")

    def test_a_link_whose_target_is_not_kept(self):
        self.assert_refused(self.llvm_entry(keep=["bin/clang++"], rename={}),
                            "hard link")

    def test_a_dangling_symlink(self):
        self.assert_refused(self.llvm_entry(keep=["bin/clang"], rename={}),
                            "links to something not kept: bin/clang")

    def test_a_path_outside_the_tree(self):
        data = tar_xz([("top/../../evil", "file", b"x")])
        self.assert_refused(self.llvm_entry(sha256=self.offer(data), keep=None),
                            "unsafe path")

    def test_a_symlink_outside_the_tree(self):
        data = tar_xz([("top/bin/clang", "symlink", "../../../etc/passwd")])
        self.assert_refused(self.llvm_entry(sha256=self.offer(data), keep=None, rename={}),
                            "links outside the tree")

    def test_a_version_check_that_fails(self):
        entry = self.llvm_entry()
        entry["check"]["expect"] = "clang version 99"
        self.assert_refused(entry, "expected 'clang version 99'")


class FakeResponse(io.BytesIO):
    def __init__(self, data: bytes, status: int, length: int):
        super().__init__(data)
        self.status = status
        self.headers = {"Content-Length": str(length)}


class DownloadTest(Fixture):
    def setUp(self):
        super().setUp()
        self.data = bytes(range(256)) * 1000
        self.source = {"url": "https://example.invalid/a.tar.xz", "sha256": sha256(self.data)}
        self.offsets = []
        sleep = mock.patch.object(fetch.time, "sleep")
        sleep.start()
        self.addCleanup(sleep.stop)

    def serve(self, cut: int):
        """A server whose connection drops after `cut` bytes, without an error."""
        def open_url(source, offset):
            self.offsets.append(offset)
            rest = self.data[offset:]
            return FakeResponse(rest[:cut], 206 if offset else 200, len(rest))
        return mock.patch.object(fetch, "open_url", open_url)

    def test_a_short_transfer_is_continued_where_it_stopped(self):
        with self.serve(cut=100_000), contextlib.redirect_stdout(io.StringIO()):
            path = fetch.download(self.source, self.downloads)
        self.assertEqual(path.read_bytes(), self.data)
        self.assertEqual(self.offsets, [0, 100_000, 200_000])

    def test_a_wrong_hash_leaves_nothing_behind(self):
        self.source["sha256"] = "0" * 64
        with self.serve(cut=len(self.data)), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(fetch.FetchError):
                fetch.download(self.source, self.downloads)
        self.assertEqual(list(self.downloads.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
