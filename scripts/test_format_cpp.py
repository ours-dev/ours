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

import format_cpp

REPO = Path(__file__).resolve().parent.parent
try:
    CLANG_FORMAT = format_cpp.find_clang_format(REPO)
except format_cpp.FormatError:
    CLANG_FORMAT = None

BAD = "int   a =1;\n"          # what clang-format changes
GOOD = "int a = 1;\n"


class Pure(unittest.TestCase):
    def test_changed_ranges(self):
        diff = "@@ -1,2 +1,3 @@\n+x\n@@ -9 +10 @@\n-y\n+z\n@@ -20,3 +21,0 @@\n-gone\n"
        self.assertEqual(format_cpp.changed_ranges(diff), [(1, 3), (10, 10)])  # a pure deletion adds no lines

    def test_diff_section_follows_a_rename(self):
        diff = ("diff --git a/a.cpp b/a.cpp\n--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n-x\n+y\n"
                "diff --git a/old.cpp b/new.cpp\nsimilarity index 100%\nrename from old.cpp\nrename to new.cpp\n"
                "diff --git a/c.cpp b/moved.cpp\nrename from c.cpp\nrename to moved.cpp\n--- a/c.cpp\n+++ b/moved.cpp\n@@ -5 +5 @@\n-p\n+q\n")
        self.assertEqual(format_cpp.changed_ranges(format_cpp.diff_section(diff, "a.cpp")), [(1, 1)])
        self.assertEqual(format_cpp.changed_ranges(format_cpp.diff_section(diff, "new.cpp")), [])   # moved, not edited
        self.assertEqual(format_cpp.changed_ranges(format_cpp.diff_section(diff, "moved.cpp")), [(5, 5)])
        self.assertEqual(format_cpp.diff_section(diff, "other.cpp"), "")


@unittest.skipUnless(CLANG_FORMAT, "the repo's prebuilt clang-format is not available")
class InARepository(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name).resolve()
        self.git("init", "-q")
        (self.top / ".clang-format").write_text((REPO / ".clang-format").read_text())
        (self.top / ".clang-format-ignore").write_text("# comment\nskipme/*.hpp\n")
        (self.top / ".gitmodules").write_text('[submodule "sub"]\n\tpath = sub\n\turl = x\n')

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=self.top,
                              capture_output=True, text=True, check=True).stdout

    def write(self, rel, text):
        p = self.top / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def commit(self, *rels):
        self.git("add", ".clang-format", *rels)
        self.git("commit", "-q", "-m", "x")

    def fmt(self, path, **kw):
        return format_cpp.format_file(path, clang_format=CLANG_FORMAT, **kw)

    def test_only_changed_lines_of_a_tracked_file(self):
        p = self.write("legacy.cpp", BAD + BAD)         # two badly formatted lines, committed
        self.commit("legacy.cpp")
        p.write_text(BAD + BAD + BAD)                    # the third line is the agent's
        status, detail = self.fmt(p)
        self.assertEqual(status, "formatted")
        self.assertEqual(p.read_text(), BAD + BAD + GOOD)   # legacy lines are left exactly as they were
        self.assertIn("3-3", detail)

    def test_a_new_file_is_formatted_whole(self):
        p = self.write("new.cpp", BAD + BAD)
        self.assertEqual(self.fmt(p)[0], "formatted")
        self.assertEqual(p.read_text(), GOOD + GOOD)

    def test_an_unchanged_tracked_file_is_not_touched(self):
        p = self.write("legacy.cpp", BAD)
        self.commit("legacy.cpp")
        self.assertEqual(self.fmt(p), ("clean", "no changed lines"))
        self.assertEqual(p.read_text(), BAD)

    def test_a_moved_file_is_only_formatted_where_it_was_edited(self):
        p = self.write("old.cpp", BAD + BAD + BAD)
        self.commit("old.cpp")
        self.git("mv", "old.cpp", "moved.cpp")
        moved = self.top / "moved.cpp"
        self.assertEqual(self.fmt(moved)[0], "clean")     # a pure move is not an edit
        moved.write_text(BAD + "// note\n" + BAD + BAD)
        self.assertEqual(self.fmt(moved)[0], "clean")     # the comment line is already fine
        moved.write_text(BAD + BAD + BAD + BAD)
        self.assertEqual(self.fmt(moved)[0], "formatted")
        self.assertEqual(moved.read_text(), BAD + BAD + BAD + GOOD)

    def test_files_that_are_not_ours_are_skipped(self):
        for rel, why in [("vendor/x.cpp", "third-party"), ("out/gen/x.cpp", "third-party"), ("sub/y.cpp", "submodule"),
                         ("skipme/asm.hpp", "clang-format-ignore"), ("notes.md", "not C/C++")]:
            with self.subTest(rel=rel):
                p = self.write(rel, BAD)
                status, detail = self.fmt(p)
                self.assertEqual(status, "skipped")
                self.assertIn(why, detail)
                self.assertEqual(p.read_text(), BAD)

    def test_a_file_outside_any_repository_is_skipped(self):
        with tempfile.TemporaryDirectory() as elsewhere:
            p = Path(elsewhere) / "x.cpp"
            p.write_text(BAD)
            self.assertEqual(self.fmt(p)[0], "skipped")
            self.assertEqual(p.read_text(), BAD)

    def test_check_mode_reports_and_changes_nothing(self):
        p = self.write("new.cpp", BAD)
        self.assertEqual(self.fmt(p, check_only=True), ("formatted", "would change"))
        self.assertEqual(p.read_text(), BAD)

    def test_a_file_without_a_config_is_left_alone(self):
        (self.top / ".clang-format").unlink()
        p = self.write("new.cpp", BAD)
        self.assertEqual(self.fmt(p)[0], "clean")
        self.assertEqual(p.read_text(), BAD)

    def hook(self, payload):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"CLANG_FORMAT": CLANG_FORMAT}), contextlib.redirect_stdout(out):
            code = format_cpp.claude_hook(json.dumps(payload))
        return code, out.getvalue()

    def test_claude_hook_formats_and_tells_the_model(self):
        p = self.write("new.cpp", BAD)
        code, out = self.hook({"tool_name": "Write", "tool_input": {"file_path": str(p)}})
        self.assertEqual(code, 0)
        self.assertEqual(p.read_text(), GOOD)
        note = json.loads(out)["hookSpecificOutput"]
        self.assertEqual(note["hookEventName"], "PostToolUse")
        self.assertIn("Read it again", note["additionalContext"])

    def test_claude_hook_is_silent_when_there_is_nothing_to_do(self):
        clean = self.write("clean.cpp", GOOD)
        for payload in [{"tool_input": {"file_path": str(clean)}}, {"tool_input": {"file_path": str(self.write("a.md", BAD))}},
                        {"tool_input": {}}, {}, {"tool_input": {"file_path": str(self.top / "missing.cpp")}}]:
            with self.subTest(payload=payload):
                self.assertEqual(self.hook(payload), (0, ""))
        self.assertEqual(self.hook_raw("not json"), (0, ""))

    def hook_raw(self, text):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = format_cpp.claude_hook(text)
        return code, out.getvalue()

    def test_a_relative_path_is_resolved_against_the_payload_cwd(self):
        p = self.write("new.cpp", BAD)
        self.hook({"tool_input": {"file_path": "new.cpp"}, "cwd": str(self.top)})
        self.assertEqual(p.read_text(), GOOD)


class ToolDiscovery(unittest.TestCase):
    def test_env_override_wins(self):
        with mock.patch.dict(os.environ, {"CLANG_FORMAT": "/somewhere/clang-format"}):
            self.assertEqual(format_cpp.find_clang_format(REPO), "/somewhere/clang-format")

    def test_a_missing_tool_is_reported_not_hidden(self):
        with tempfile.TemporaryDirectory() as empty, mock.patch.dict(os.environ, {"PATH": ""}):
            os.environ.pop("CLANG_FORMAT", None)
            with self.assertRaises(format_cpp.FormatError):
                format_cpp.find_clang_format(Path(empty))


if __name__ == "__main__":
    unittest.main()
