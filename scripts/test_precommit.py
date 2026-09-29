import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import format_cpp

REPO = Path(__file__).resolve().parent.parent
try:
    CLANG_FORMAT = format_cpp.find_clang_format(REPO)
except format_cpp.FormatError:
    CLANG_FORMAT = None

BAD = "int   a =1;\n"
GOOD = "int a = 1;\n"
TOOLS = ["format_cpp.py", "lint_docs.py", "claude_hook.py", "gitdiff.py"]
TEMPLATES = ["docs/changes/TEMPLATE.md", "docs/repairs/TEMPLATE.md", ".agents/tasks/TEMPLATE.md"]


@unittest.skipUnless(CLANG_FORMAT, "the repo's prebuilt clang-format is not available")
class PreCommit(unittest.TestCase):
    """The real hook, in a scratch repository, driven through real `git commit`s."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name).resolve()
        self.git("init", "-q")
        for rel in [".clang-format", ".githooks/pre-commit", *TEMPLATES, *[f"scripts/{t}" for t in TOOLS]]:
            (self.top / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, self.top / rel)
        # Found the way it is found in the real tree, not through CLANG_FORMAT: under a hook git
        # exports GIT_DIR, and a tool that computed the repository root wrongly would not find it.
        binary = self.top / "prebuilt/clang/llvm-23/bin/clang-format"
        binary.parent.mkdir(parents=True)
        binary.symlink_to(CLANG_FORMAT)
        self.git("config", "core.hooksPath", ".githooks")
        self.git("config", "gc.auto", "0")               # background maintenance must not race tearDown
        self.git("config", "maintenance.auto", "false")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "base")
        self.env = {k: v for k, v in os.environ.items() if k != "CLANG_FORMAT"}

    def tearDown(self):
        shutil.rmtree(self.tmp.name, ignore_errors=True)

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

    def test_a_file_in_a_subdirectory_is_checked_against_the_right_root(self):
        """The regression. In a linked worktree, `git commit -- path` exports GIT_DIR to the hook with
        no GIT_WORK_TREE, and `git rev-parse --show-toplevel` run from a subdirectory then answers
        with that subdirectory. The tool took it for the root, found no prebuilt clang-format, and the
        hook reported 'format check skipped' while the commit went through unchecked."""
        wt = self.top.parent / (self.top.name + "-wt")
        self.git("worktree", "add", "-q", str(wt), "-b", "other")
        self.addCleanup(shutil.rmtree, wt, ignore_errors=True)
        p = wt / "kernel/lib/deep/a.cpp"
        p.parent.mkdir(parents=True)
        p.write_text(BAD)
        run = lambda *args: subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=wt,  # noqa: E731
                                           capture_output=True, text=True, env=self.env)
        run("add", "-N", "kernel/lib/deep/a.cpp")
        refused = run("commit", "-m", "x", "--", "kernel/lib/deep/a.cpp")
        self.assertNotEqual(refused.returncode, 0, refused.stderr)
        self.assertIn("kernel/lib/deep/a.cpp", refused.stderr)
        self.assertNotIn("skipped", refused.stderr)
        p.write_text(GOOD)
        self.assertEqual(run("commit", "-m", "x", "--", "kernel/lib/deep/a.cpp").returncode, 0)

    def test_a_file_in_a_subdirectory_of_a_plain_repository(self):
        bad = self.write("kernel/lib/deep/a.cpp", BAD)
        result = self.commit("kernel/lib/deep/a.cpp")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kernel/lib/deep/a.cpp", result.stderr)
        bad.write_text(GOOD)
        self.assertEqual(self.commit("kernel/lib/deep/a.cpp").returncode, 0)

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

    def test_both_checks_report_in_one_go(self):
        self.write("a.cpp", BAD)
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("a.cpp", "docs/notes.md")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clang-format", result.stderr)
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

    def test_a_worktree_without_the_scripts_is_not_blocked(self):
        shutil.rmtree(self.top / "scripts")
        self.write("a.cpp", BAD)
        self.write("docs/notes.md", "# Notes\n\ntrailing \n")
        result = self.commit("a.cpp", "docs/notes.md")
        self.assertEqual(result.returncode, 0)
        self.assertIn("format check skipped", result.stderr)
        self.assertIn("document check skipped", result.stderr)

    def test_other_files_are_not_looked_at(self):
        self.write("notes.txt", "trailing \n\ttab\n")
        self.write("BUILD.gn", "a  =  1 \n")
        self.assertEqual(self.commit("notes.txt", "BUILD.gn").returncode, 0)


if __name__ == "__main__":
    unittest.main()
