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

import install_hooks

REPO = Path(__file__).resolve().parent.parent
FILES = list(install_hooks.REQUIRED)


class InstallHooks(unittest.TestCase):
    """Real repositories: a main worktree that has adopted the hook, and siblings that may not have."""

    def setUp(self):
        self.base = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)
        self.main = self.base / "main"
        self.main.mkdir()
        self.git(self.main, "init", "-q")
        self.git(self.main, "config", "gc.auto", "0")
        for rel in FILES:
            (self.main / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, self.main / rel)
        self.git(self.main, "add", "-A")
        self.git(self.main, "commit", "-q", "-m", "adopted")

    def git(self, cwd, *args):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=cwd, capture_output=True, text=True)

    def worktree(self, name, adopted=True):
        path = self.base / name
        self.git(self.main, "worktree", "add", "-q", str(path), "-b", name)
        if not adopted:
            for rel in FILES[1:]:                      # an older branch: has the old pre-commit, none of the scripts
                (path / rel).unlink()
            (path / ".githooks/pre-commit").write_text("#!/usr/bin/env python3\n# formats whole files itself\n")
        return path

    def effective(self, path):
        return self.git(path, "config", "--get", "core.hooksPath").stdout.strip() or None

    def test_a_worktree_that_has_adopted_the_hook_is_enabled(self):
        wt = self.worktree("a")
        self.assertEqual(install_hooks.enable(wt)[0], "enabled")
        self.assertEqual(self.effective(wt), ".githooks")

    def test_the_setting_is_per_worktree_and_siblings_are_untouched(self):
        a, b = self.worktree("a"), self.worktree("b", adopted=False)
        install_hooks.enable(a)
        self.assertEqual(self.effective(a), ".githooks")
        self.assertIsNone(self.effective(b))                # a sibling on an older branch would have run the old hook
        self.assertIsNone(self.effective(self.main))

    def test_the_main_worktree_can_be_enabled_too(self):
        self.assertEqual(install_hooks.enable(self.main)[0], "enabled")
        self.assertEqual(self.effective(self.main), ".githooks")

    def test_enabling_twice_changes_nothing_the_second_time(self):
        wt = self.worktree("a")
        install_hooks.enable(wt)
        self.assertEqual(install_hooks.enable(wt), ("already", "core.hooksPath is .githooks"))

    def test_a_worktree_that_predates_the_hook_is_left_alone(self):
        old = self.worktree("old", adopted=False)
        status, detail = install_hooks.enable(old)
        self.assertEqual(status, "unavailable")
        self.assertIn("scripts/format_cpp.py", detail)
        self.assertIsNone(self.effective(old))

    def test_an_old_pre_commit_alone_is_not_enough(self):
        wt = self.worktree("half")
        (wt / ".githooks/pre-commit").write_text("#!/usr/bin/env python3\n# formats whole files itself\n")
        status, detail = install_hooks.enable(wt)
        self.assertEqual(status, "unavailable")
        self.assertIn("old pre-commit", detail)

    def test_another_hooks_path_is_never_overridden(self):
        wt = self.worktree("a")
        self.git(wt, "config", "core.hooksPath", "tools/husky")
        status, detail = install_hooks.enable(wt)
        self.assertEqual(status, "conflict")
        self.assertIn("tools/husky", detail)
        self.assertEqual(self.effective(wt), "tools/husky")

    def test_check_reports_and_changes_nothing(self):
        wt = self.worktree("a")
        self.assertEqual(install_hooks.enable(wt, check_only=True), ("unavailable", "not enabled"))
        self.assertIsNone(self.effective(wt))

    def run_cli(self, cwd, *args):
        out = io.StringIO()
        with contextlib.chdir(cwd) if hasattr(contextlib, "chdir") else _chdir(cwd), contextlib.redirect_stdout(out):
            code = install_hooks.main(list(args))
        return code, out.getvalue()

    def test_the_command_line(self):
        wt = self.worktree("a")
        self.assertEqual(self.run_cli(wt, "--check")[0], 1)
        code, out = self.run_cli(wt)
        self.assertEqual((code, out.split(":")[0]), (0, "enabled"))
        self.assertEqual(self.run_cli(wt, "--check")[0], 0)
        self.assertEqual(self.run_cli(self.worktree("old", adopted=False))[0], 1)

    def test_the_session_start_hook_speaks_only_when_something_changed(self):
        wt = self.worktree("a")
        code, out = self.run_cli(wt, "--claude-hook")
        self.assertEqual(code, 0)
        self.assertIn("Enabled this worktree's git hooks", json.loads(out)["systemMessage"])
        self.assertEqual(self.run_cli(wt, "--claude-hook"), (0, ""))                      # already on: silent
        self.assertEqual(self.run_cli(self.worktree("old", adopted=False), "--claude-hook"), (0, ""))   # predates: silent
        clash = self.worktree("clash")
        self.git(clash, "config", "core.hooksPath", "elsewhere")
        self.assertIn("elsewhere", json.loads(self.run_cli(clash, "--claude-hook")[1])["systemMessage"])

    def test_outside_a_repository_the_hook_does_nothing(self):
        with tempfile.TemporaryDirectory() as elsewhere:
            self.assertEqual(self.run_cli(Path(elsewhere), "--claude-hook"), (0, ""))
            self.assertEqual(self.run_cli(Path(elsewhere))[0], 1)

    def test_enabled_hooks_actually_run_in_that_worktree(self):
        """End to end: after enabling, git runs that worktree's own pre-commit."""
        wt = self.worktree("a")
        install_hooks.enable(wt)
        (wt / "docs").mkdir()
        (wt / "docs/x.md").write_text("# x\n\ntrailing \n")
        self.git(wt, "add", "docs/x.md")
        result = self.git(wt, "commit", "-m", "x")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("trailing-whitespace", result.stderr)

    def test_a_half_adopted_set_of_scripts_is_not_enabled(self):
        """The scripts exist but one cannot run: enabling would make the hook refuse every commit."""
        wt = self.worktree("half")
        (wt / "scripts/claude_hook.py").write_text("raise SystemExit('broken')\n")
        status, detail = install_hooks.enable(wt)
        self.assertEqual(status, "unavailable")
        self.assertIn("does not run here", detail)
        self.assertIsNone(self.effective(wt))


@contextlib.contextmanager
def _chdir(path):
    old = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


if __name__ == "__main__":
    unittest.main()
