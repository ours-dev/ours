import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import format_gn
from formatting import FormatError

REPO = Path(__file__).resolve().parent.parent
try:
    GN = format_gn.find_gn(REPO)
except FormatError:
    GN = None

MESSY = 'group( "x" ){\ndeps=[":b",":a"]\n}\n'
# gn's layout, and the lists still in the order they were written (plain gn would sort :a first).
MESSY_FORMATTED = 'group("x") {\n  deps = [\n    ":b",\n    ":a",\n  ]\n}\n'
BROKEN = 'group("x" {\n'


@unittest.skipUnless(GN, "the repo's prebuilt gn is not available")
class InARepository(unittest.TestCase):
    def setUp(self):
        self.top = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.top, ignore_errors=True)
        subprocess.run(["git", "init", "-q"], cwd=self.top, check=True)
        (self.top / ".gitmodules").write_text('[submodule "sub"]\n\tpath = sub\n\turl = x\n')

    def write(self, rel, text):
        p = self.top / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def fmt(self, path, **kw):
        return format_gn.format_file(path, gn=GN, **kw)

    def test_layout_is_normalised_and_lists_keep_their_order(self):
        p = self.write("BUILD.gn", MESSY)
        self.assertEqual(self.fmt(p), ("formatted", "reformatted the whole file"))
        self.assertEqual(p.read_text(), MESSY_FORMATTED)
        self.assertEqual(self.fmt(p)[0], "clean")           # and it is stable

    def test_sources_and_deps_are_never_sorted(self):
        """The reason this tool exists: sorting reorders the link line and the -I directories."""
        src = 'static_library("x") {\n  sources = [ "z.cpp", "a.cpp" ]\n  deps = [ "//k/b", "//k/a" ]\n}\n'
        p = self.write("BUILD.gn", src)
        self.fmt(p)
        text = p.read_text()
        self.assertLess(text.index('"z.cpp"'), text.index('"a.cpp"'))
        self.assertLess(text.index('"//k/b"'), text.index('"//k/a"'))

    def test_the_marker_never_survives(self):
        p = self.write("BUILD.gn", MESSY)
        self.fmt(p)
        self.assertNotIn("NOSORT", p.read_text())

    def test_statements_do_not_gain_blank_lines(self):
        """A comment above an assignment makes gn add a blank line; the marker must not leave one behind."""
        clean = 'group("x") {\n  testonly = true\n  public_deps = [ ":a" ]\n  visibility = [ "*" ]\n}\n'
        p = self.write("BUILD.gn", clean)
        self.assertEqual(self.fmt(p), ("clean", "already formatted"))
        self.assertEqual(p.read_text(), clean)

    def test_a_marker_the_author_wrote_stays(self):
        src = 'group("x") {\n  # NOSORT\n  deps = [\n    ":b",\n    ":a",\n  ]\n}\n'
        p = self.write("BUILD.gn", src)
        self.assertEqual(self.fmt(p)[0], "clean")
        self.assertEqual(p.read_text(), src)

    def test_a_comment_above_a_list_is_kept_and_only_the_marker_goes(self):
        src = 'group("x") {\n  # why b comes first\n  deps = [ ":b", ":a" ]\n}\n'
        p = self.write("BUILD.gn", src)
        self.fmt(p)
        self.assertEqual(p.read_text(), 'group("x") {\n  # why b comes first\n  deps = [\n    ":b",\n    ":a",\n  ]\n}\n')

    def test_a_rewrite_that_is_more_than_layout_is_refused(self):
        cases = {
            "label shorthand": 'group("x") {\n  deps = [ "//foo:foo" ]\n}\n',   # gn shortens it to "//foo"
            "import order": 'import("//b.gni")\nimport("//a.gni")\n',           # gn sorts the imports
            "redundant parentheses": 'x = (1 + 2)\n',                            # gn drops them
        }
        for name, src in cases.items():
            with self.subTest(name):
                p = self.write("BUILD.gn", src)
                status, detail = self.fmt(p)
                self.assertEqual(status, "unsafe")
                self.assertIn("more than layout", detail)
                self.assertEqual(p.read_text(), src)            # untouched

    def test_an_assignment_sharing_a_line_with_other_code_is_left_alone(self):
        """The markers go on lines; a one-line block has no line to put one on, and guessing would
        risk the sort this tool exists to prevent."""
        src = 'group( "x" ){ deps=[":b",":a"] }\n'
        p = self.write("BUILD.gn", src)
        status, detail = self.fmt(p)
        self.assertEqual(status, "unsafe")
        self.assertIn("could not line up", detail)
        self.assertEqual(p.read_text(), src)

    def test_gni_and_the_root_dotfile_are_gn_files_too(self):
        for rel in ("build/flags.gni", ".gn", "kernel/lib/x/BUILD.gn"):
            with self.subTest(rel=rel):
                p = self.write(rel, MESSY)
                self.assertEqual(self.fmt(p)[0], "formatted")

    def test_check_mode_reports_and_changes_nothing(self):
        p = self.write("BUILD.gn", MESSY)
        self.assertEqual(self.fmt(p, check_only=True), ("formatted", "would change"))
        self.assertEqual(p.read_text(), MESSY)

    def test_a_file_edited_while_it_was_formatted_is_not_overwritten(self):
        """Another agent can write between our read and our write; theirs must win."""
        p = self.write("BUILD.gn", MESSY)
        real = format_gn.format_keeping_order

        def edit_meanwhile(*args):
            result = real(*args)
            p.write_text(MESSY + "# another agent was here\n")
            return result

        with mock.patch.object(format_gn, "format_keeping_order", edit_meanwhile):
            status, detail = self.fmt(p)
        self.assertEqual(status, "skipped")
        self.assertIn("changed while formatting", detail)
        self.assertEqual(p.read_text(), MESSY + "# another agent was here\n")

    def test_a_file_that_does_not_parse_is_reported_and_untouched(self):
        p = self.write("BUILD.gn", BROKEN)
        status, detail = self.fmt(p)
        self.assertEqual(status, "invalid")
        self.assertIn("ERROR at BUILD.gn:1:", detail)
        self.assertIn("Expected comma", detail)
        self.assertEqual(p.read_text(), BROKEN)

    def test_files_that_are_not_ours_are_skipped(self):
        for rel, why in [("vendor/x/BUILD.gn", "third-party"), ("out/gen/args.gn", "third-party"), ("sub/BUILD.gn", "submodule"),
                         ("kernel/notes.txt", "not a GN file"), ("kernel/gn.py", "not a GN file")]:
            with self.subTest(rel=rel):
                p = self.write(rel, MESSY)
                status, detail = self.fmt(p)
                self.assertEqual(status, "skipped")
                self.assertIn(why, detail)
                self.assertEqual(p.read_text(), MESSY)

    def test_a_file_outside_any_repository_is_skipped(self):
        with tempfile.TemporaryDirectory() as elsewhere:
            p = Path(elsewhere) / "BUILD.gn"
            p.write_text(MESSY)
            self.assertEqual(self.fmt(p)[0], "skipped")
            self.assertEqual(p.read_text(), MESSY)

    def test_a_gn_that_cannot_be_started_is_a_tool_failure(self):
        p = self.write("BUILD.gn", MESSY)
        with self.assertRaises(FormatError):
            format_gn.format_file(p, gn="/nonexistent/gn")

    def hook(self, payload):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"GN": GN}), contextlib.redirect_stdout(out):
            code = format_gn.claude_hook(json.dumps(payload))
        return code, out.getvalue()

    def test_the_hook_formats_and_tells_the_model(self):
        p = self.write("BUILD.gn", MESSY)
        code, out = self.hook({"tool_input": {"file_path": str(p)}})
        self.assertEqual(code, 0)
        self.assertEqual(p.read_text(), MESSY_FORMATTED)
        self.assertIn("Read it again", json.loads(out)["hookSpecificOutput"]["additionalContext"])

    def test_the_hook_blocks_when_the_edit_broke_the_file(self):
        p = self.write("BUILD.gn", BROKEN)
        code, out = self.hook({"tool_input": {"file_path": str(p)}})
        answer = json.loads(out)
        self.assertEqual((code, answer["decision"]), (0, "block"))
        self.assertIn("no longer parses", answer["reason"])

    def test_the_hook_says_so_when_it_declines_to_format(self):
        p = self.write("BUILD.gn", 'import("//b.gni")\nimport("//a.gni")\n')
        code, out = self.hook({"tool_input": {"file_path": str(p)}})
        self.assertEqual(code, 0)
        self.assertIn("was not applied", json.loads(out)["hookSpecificOutput"]["additionalContext"])

    def test_the_hook_is_silent_when_there_is_nothing_to_say(self):
        clean = self.write("BUILD.gn", MESSY_FORMATTED)
        for payload in [{"tool_input": {"file_path": str(clean)}}, {"tool_input": {"file_path": str(self.write("a.md", MESSY))}},
                        {"tool_input": {}}, {}]:
            with self.subTest(payload=payload):
                self.assertEqual(self.hook(payload), (0, ""))


class ToolDiscovery(unittest.TestCase):
    def test_env_override_wins(self):
        with mock.patch.dict(os.environ, {"GN": "/somewhere/gn"}):
            self.assertEqual(format_gn.find_gn(REPO), "/somewhere/gn")

    def test_a_missing_tool_is_reported_not_hidden(self):
        with tempfile.TemporaryDirectory() as empty, mock.patch.dict(os.environ, {"PATH": ""}):
            os.environ.pop("GN", None)
            with self.assertRaises(FormatError):
                format_gn.find_gn(Path(empty))

    def test_which_files_are_gn(self):
        self.assertTrue(all(format_gn.is_gn_file(p) for p in ["BUILD.gn", "a/b/c.gni", ".gn", "kernel/.gn"]))
        self.assertFalse(any(format_gn.is_gn_file(p) for p in ["gn", "x.gnu", "BUILD.gn.orig", "gni", "a/gn.py"]))


class TheTokenGuard(unittest.TestCase):
    def test_layout_commas_and_comments_do_not_count(self):
        a = 'deps = [ ":a", ":b" ]  # why\n'
        b = 'deps = [\n  ":a",\n  ":b"\n]\n'
        self.assertEqual(format_gn.tokens(a), format_gn.tokens(b))

    def test_order_does_count(self):
        self.assertNotEqual(format_gn.tokens('x = [ "a", "b" ]'), format_gn.tokens('x = [ "b", "a" ]'))

    def test_a_hash_inside_a_string_is_not_a_comment(self):
        self.assertIn('"a#b"', format_gn.tokens('x = "a#b"  # real comment'))
        self.assertNotIn("#", "".join(t for t in format_gn.tokens('x = 1 # real') if t.startswith("#")))


if __name__ == "__main__":
    unittest.main()
