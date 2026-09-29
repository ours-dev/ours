import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import format_cpp
import format_gn
import install_hooks

REPO = Path(__file__).resolve().parent.parent
try:
    CLANG_FORMAT = format_cpp.find_clang_format(REPO)
    GN = format_gn.find_gn(REPO)
except format_cpp.FormatError:
    CLANG_FORMAT = GN = None

BAD = "int   a =1;\n"
GOOD = "int a = 1;\n"
GN_BAD = 'group( "x" ){\ndeps=[":b",":a"]\n}\n'
GN_GOOD = 'group("x") {\n  deps = [\n    ":b",\n    ":a",\n  ]\n}\n'  # :b before :a on purpose
GN_BROKEN = 'group("x" {\n'
GN_BEYOND_LAYOUT = 'import("//b.gni")\nimport("//a.gni")\n'  # gn would reorder the imports, which is more than layout
TEMPLATES = ["docs/changes/TEMPLATE.md", "docs/repairs/TEMPLATE.md", ".agents/tasks/TEMPLATE.md"]


@unittest.skipUnless(CLANG_FORMAT and GN, "the repo's prebuilt clang-format and gn are not available")
class PreCommit(unittest.TestCase):
    """The real hook, in a scratch repository, driven through real `git commit`s."""

    def setUp(self):
        self.top = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.top, ignore_errors=True)
        self.git("init", "-q")
        # What the hook needs is install_hooks.REQUIRED, the list a worktree is judged by. A copy kept
        # here went stale when format_cpp.py gained an import, and every check then "skipped".
        for rel in [".clang-format", *TEMPLATES, *install_hooks.REQUIRED]:
            (self.top / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, self.top / rel)
        # Found the way it is found in the real tree, not through CLANG_FORMAT: under a hook git
        # exports GIT_DIR, and a tool that computed the repository root wrongly would not find it.
        for rel, tool in (("prebuilt/clang/llvm-23/bin/clang-format", CLANG_FORMAT), ("prebuilt/gn/gn", GN)):
            (self.top / rel).parent.mkdir(parents=True)
            (self.top / rel).symlink_to(tool)
        self.git("config", "core.hooksPath", ".githooks")
        self.git("config", "gc.auto", "0")               # background maintenance must not race tearDown
        self.git("config", "maintenance.auto", "false")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "base")
        self.env = {k: v for k, v in os.environ.items() if k not in ("CLANG_FORMAT", "GN")}

    def git(self, *args, env=None, check=True):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=self.top, capture_output=True,
                              text=True, check=check, env=env or getattr(self, "env", None))

    def write(self, rel, text):
        p = self.top / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def commit(self, *paths, env=None):
        """Stage `paths` and commit; returns the CompletedProcess without raising."""
        self.git("add", *paths)
        return self.git("commit", "-m", "x", check=False, env=env)

    def test_clean_work_is_committed(self):
        self.write("a.cpp", GOOD)
        self.write("docs/notes.md", "# Notes\n\nText.\n")
        self.assertEqual(self.commit("a.cpp", "docs/notes.md").returncode, 0)

    def test_an_unformatted_change_is_refused_and_nothing_is_modified(self):
        p = self.write("a.cpp", BAD)
        result = self.commit("a.cpp")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("a.cpp", result.stderr)
        self.assertIn("python3 scripts/format_cpp.py a.cpp", result.stderr)
        self.assertEqual(p.read_text(), BAD)                       # the hook reports, it never reformats
        self.assertEqual(self.git("diff", "--cached", "--name-only").stdout.split(), ["a.cpp"])   # still staged as it was

    def test_the_suggested_fix_gets_the_commit_through(self):
        p = self.write("a.cpp", BAD)
        self.assertNotEqual(self.commit("a.cpp").returncode, 0)
        subprocess.run([sys.executable, "scripts/format_cpp.py", "a.cpp"], cwd=self.top, env=self.env, check=True, capture_output=True)
        self.assertEqual(p.read_text(), GOOD)
        self.assertEqual(self.commit("a.cpp").returncode, 0)

    def test_legacy_lines_are_not_blamed(self):
        p = self.write("legacy.cpp", BAD + BAD)
        self.git("add", "legacy.cpp")
        self.git("commit", "-q", "--no-verify", "-m", "legacy")
        p.write_text(BAD + BAD + GOOD)                              # a clean line added to a dirty file
        self.assertEqual(self.commit("legacy.cpp").returncode, 0)

    def test_the_commit_only_paths_form_is_checked_too(self):
        self.write("a.cpp", BAD)
        self.write("b.cpp", GOOD)
        self.git("add", "-N", "a.cpp", "b.cpp")
        refused = self.git("commit", "-m", "x", "--", "a.cpp", check=False)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("a.cpp", refused.stderr)
        self.assertEqual(self.git("commit", "-m", "x", "--", "b.cpp", check=False).returncode, 0)

    def linked_worktree(self):
        """A second worktree of the scratch repository, and a function running git in it."""
        wt = self.top.parent / (self.top.name + "-wt")
        self.git("worktree", "add", "-q", str(wt), "-b", "other")
        self.addCleanup(shutil.rmtree, wt, ignore_errors=True)
        run = lambda *args: subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=wt,  # noqa: E731
                                           capture_output=True, text=True, env=self.env)
        return wt, run

    def test_a_file_in_a_subdirectory_is_checked_against_the_right_root(self):
        """The regression. In a linked worktree, `git commit -- path` exports GIT_DIR to the hook with
        no GIT_WORK_TREE, and `git rev-parse --show-toplevel` run from a subdirectory then answers
        with that subdirectory. The tool took it for the root, found no prebuilt clang-format, and the
        hook reported 'format check skipped' while the commit went through unchecked."""
        wt, run = self.linked_worktree()
        p = wt / "kernel/lib/deep/a.cpp"
        p.parent.mkdir(parents=True)
        p.write_text(BAD)
        run("add", "-N", "kernel/lib/deep/a.cpp")
        refused = run("commit", "-m", "x", "--", "kernel/lib/deep/a.cpp")
        self.assertNotEqual(refused.returncode, 0, refused.stderr)
        self.assertIn("kernel/lib/deep/a.cpp", refused.stderr)
        self.assertNotIn("skipped", refused.stderr)
        p.write_text(GOOD)
        self.assertEqual(run("commit", "-m", "x", "--", "kernel/lib/deep/a.cpp").returncode, 0)

    def test_a_gn_file_in_a_subdirectory_of_a_linked_worktree_is_checked_too(self):
        wt, run = self.linked_worktree()
        p = wt / "kernel/lib/deep/BUILD.gn"
        p.parent.mkdir(parents=True)
        p.write_text(GN_BAD)
        run("add", "-N", "kernel/lib/deep/BUILD.gn")
        refused = run("commit", "-m", "x", "--", "kernel/lib/deep/BUILD.gn")
        self.assertNotEqual(refused.returncode, 0, refused.stderr)
        self.assertIn("kernel/lib/deep/BUILD.gn", refused.stderr)
        self.assertNotIn("skipped", refused.stderr)
        p.write_text(GN_GOOD)
        self.assertEqual(run("commit", "-m", "x", "--", "kernel/lib/deep/BUILD.gn").returncode, 0)

    def test_a_file_in_a_subdirectory_of_a_plain_repository(self):
        bad = self.write("kernel/lib/deep/a.cpp", BAD)
        result = self.commit("kernel/lib/deep/a.cpp")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kernel/lib/deep/a.cpp", result.stderr)
        bad.write_text(GOOD)
        self.assertEqual(self.commit("kernel/lib/deep/a.cpp").returncode, 0)

    def test_a_gn_file_out_of_layout_is_refused_and_untouched(self):
        p = self.write("kernel/BUILD.gn", GN_BAD)
        result = self.commit("kernel/BUILD.gn")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kernel/BUILD.gn: formatted: would change", result.stderr)
        self.assertIn("python3 scripts/format_gn.py kernel/BUILD.gn", result.stderr)
        self.assertEqual(p.read_text(), GN_BAD)                    # reports, never reformats

    def test_the_suggested_gn_fix_gets_the_commit_through(self):
        p = self.write("kernel/BUILD.gn", GN_BAD)
        self.assertNotEqual(self.commit("kernel/BUILD.gn").returncode, 0)
        subprocess.run([sys.executable, "scripts/format_gn.py", "kernel/BUILD.gn"], cwd=self.top, env=self.env, check=True,
                       capture_output=True)
        self.assertEqual(p.read_text(), GN_GOOD)
        self.assertEqual(self.commit("kernel/BUILD.gn").returncode, 0)

    def test_gn_lists_are_not_required_to_be_sorted(self):
        self.write("BUILD.gn", GN_GOOD)
        self.assertEqual(self.commit("BUILD.gn").returncode, 0)

    def test_a_gn_file_that_does_not_parse_is_refused(self):
        self.write("BUILD.gn", GN_BROKEN)
        result = self.commit("BUILD.gn")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR at BUILD.gn:1:", result.stderr)
        self.assertNotIn("format_gn.py", result.stderr)            # there is no command that fixes a parse error

    def test_a_gn_file_gn_would_rewrite_beyond_layout_is_let_through(self):
        self.write("BUILD.gn", GN_BEYOND_LAYOUT)
        self.assertEqual(self.commit("BUILD.gn").returncode, 0)

    def test_gni_files_and_the_root_dotfile_are_checked_too(self):
        for rel in ("build/flags.gni", ".gn"):
            with self.subTest(rel=rel):
                self.write(rel, GN_BAD)
                result = self.commit(rel)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(rel, result.stderr)
                self.git("reset", "-q")                            # a refused commit leaves the file staged

    def test_a_gn_file_you_did_not_stage_cannot_fail_your_commit(self):
        self.write("elsewhere/BUILD.gn", GN_BAD)                   # someone else's unstaged work
        self.write("a.cpp", GOOD)
        self.assertEqual(self.commit("a.cpp").returncode, 0)

    def test_a_gn_check_that_cannot_run_is_reported_not_fatal(self):
        self.write("BUILD.gn", GN_BAD)
        result = self.commit("BUILD.gn", env=dict(self.env, GN="/nonexistent/gn"))
        self.assertEqual(result.returncode, 0)
        self.assertIn("GN check skipped", result.stderr)

    def test_a_nested_document_is_checked_too(self):
        self.write("docs/changes/260929/x.md", "# x changes\n\ntrailing \n")
        result = self.commit("docs/changes/260929/x.md")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("docs/changes/260929/x.md:3", result.stderr)

    def test_a_document_that_breaks_the_standard_is_refused(self):
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("docs/notes.md")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("trailing-whitespace", result.stderr)
        self.assertIn("docs/notes.md:3", result.stderr)

    def test_all_the_checks_report_in_one_go(self):
        self.write("a.cpp", BAD)
        self.write("BUILD.gn", GN_BAD)
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("a.cpp", "BUILD.gn", "docs/notes.md")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clang-format", result.stderr)
        self.assertIn("not in gn's layout", result.stderr)
        self.assertIn("layout standard", result.stderr)

    def test_a_document_you_did_not_stage_cannot_fail_your_commit(self):
        self.write("docs/elsewhere.md", "# Elsewhere\n\ntrailing \n")      # someone else's unstaged work
        self.write("a.cpp", GOOD)
        self.assertEqual(self.commit("a.cpp").returncode, 0)

    def test_a_check_that_cannot_run_is_reported_not_fatal(self):
        self.write("a.cpp", BAD)
        result = self.commit("a.cpp", env=dict(self.env, CLANG_FORMAT="/nonexistent/clang-format"))
        self.assertEqual(result.returncode, 0)
        self.assertIn("format check skipped", result.stderr)

    def test_a_tool_that_crashes_is_not_taken_for_a_verdict(self):
        (self.top / "scripts/claude_hook.py").write_text("raise SystemExit('broken')\n")
        self.write("a.cpp", BAD)
        self.write("BUILD.gn", GN_BAD)
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("a.cpp", "BUILD.gn", "docs/notes.md")
        self.assertEqual(result.returncode, 0)
        self.assertIn("format check skipped", result.stderr)
        self.assertIn("GN check skipped", result.stderr)
        self.assertIn("document check skipped", result.stderr)
        self.assertIn("broken", result.stderr)

    def test_a_worktree_without_the_scripts_is_not_blocked(self):
        shutil.rmtree(self.top / "scripts")
        self.write("a.cpp", BAD)
        self.write("BUILD.gn", GN_BAD)
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("a.cpp", "BUILD.gn", "docs/notes.md")
        self.assertEqual(result.returncode, 0)
        self.assertIn("format check skipped", result.stderr)
        self.assertIn("GN check skipped: this worktree has no scripts/format_gn.py", result.stderr)
        self.assertIn("document check skipped", result.stderr)

    def test_other_files_are_not_looked_at(self):
        self.write("notes.txt", "trailing \n\ttab\n")
        self.write("tool.py", "a  =  1 \n")
        self.assertEqual(self.commit("notes.txt", "tool.py").returncode, 0)


if __name__ == "__main__":
    unittest.main()
