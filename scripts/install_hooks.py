#!/usr/bin/env python3
"""Turn on the repo's git hooks in this worktree.

    python3 scripts/install_hooks.py            # enable (changes nothing if already on)
    python3 scripts/install_hooks.py --check    # report only; exit 1 if not enabled
    python3 scripts/install_hooks.py --claude-hook   # Claude Code SessionStart

The hooks live in .githooks/ and are switched on by core.hooksPath. That is set
per worktree, not repository-wide, on purpose: the worktrees of one repository
share a single .git/config, and several of them sit on branches that predate the
current pre-commit. Set for all, each of those would start running the old one,
which fails every commit where clang-format is not on PATH. Per worktree it goes
through extensions.worktreeConfig, which gives each worktree its own config file.

A worktree is only enabled if it has the scripts the hook calls, so adopting the
commits that bring them is what opts a worktree in, and an old branch is left
alone. The Claude Code SessionStart hook in .claude/settings.json runs this, so
a worktree with those commits turns its hooks on the first time a session opens
in it. An existing core.hooksPath that points elsewhere is never overridden.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Optional

import gitdiff

HOOKS_DIR = ".githooks"
# What the current pre-commit calls. A worktree missing any of it predates the hook.
REQUIRED = (".githooks/pre-commit", "scripts/format_cpp.py", "scripts/format_gn.py", "scripts/formatting.py", "scripts/lint_docs.py",
            "scripts/gitdiff.py", "scripts/claude_hook.py")
TOOLS = ("scripts/format_cpp.py", "scripts/format_gn.py", "scripts/lint_docs.py")
CURRENT_MARKER = "format_cpp.py"  # the old pre-commit formatted whole files itself and never mentioned this


def git(top: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=top, capture_output=True, text=True, env=gitdiff.git_env())


def worktree_top(start: Path) -> Optional[Path]:
    try:
        result = git(start, "rev-parse", "--show-toplevel")
    except OSError:
        return None
    return Path(result.stdout.strip()).resolve() if result.returncode == 0 and result.stdout.strip() else None


def hooks_path(top: Path) -> Optional[str]:
    """The core.hooksPath in effect for this worktree, wherever it is set."""
    result = git(top, "config", "--get", "core.hooksPath")
    return result.stdout.strip() or None if result.returncode == 0 else None


def predates_the_hook(top: Path) -> Optional[str]:
    for rel in REQUIRED:
        if not (top / rel).is_file():
            return f"this worktree has no {rel}"
    if CURRENT_MARKER not in (top / ".githooks/pre-commit").read_text(encoding="utf-8", errors="replace"):
        return "this worktree still has the old pre-commit"
    # Present is not the same as working: a half-adopted set of scripts fails on an import. A hook
    # that crashes would refuse every commit, so run each tool once before trusting it.
    for rel in TOOLS:
        result = subprocess.run([sys.executable, str(top / rel), "--help"], cwd=top, capture_output=True, text=True, env=gitdiff.git_env())
        if result.returncode != 0:
            return f"{rel} does not run here ({(result.stderr.strip().splitlines() or ['no message'])[-1]})"
    return None


def enable(top: Path, check_only: bool = False) -> tuple[str, str]:
    """(status, detail); status is enabled, already, unavailable or conflict."""
    current = hooks_path(top)
    if current == HOOKS_DIR:
        return "already", f"core.hooksPath is {HOOKS_DIR}"
    if current is not None:
        return "conflict", f"core.hooksPath is already {current!r}; leaving it alone"
    reason = predates_the_hook(top)
    if reason:
        return "unavailable", f"{reason}, so its git hooks stay off"
    if check_only:
        return "unavailable", "not enabled"
    steps = [("config", "extensions.worktreeConfig", "true"), ("config", "--worktree", "core.hooksPath", HOOKS_DIR)]
    for step in steps:
        result = git(top, *step)
        if result.returncode != 0:
            return "unavailable", f"git {' '.join(step)} failed: {result.stderr.strip()[:200]}"
    return ("enabled", f"core.hooksPath is now {HOOKS_DIR} in this worktree") if hooks_path(top) == HOOKS_DIR else \
        ("unavailable", "git accepted the settings but core.hooksPath did not take effect")


def claude_hook() -> int:
    """SessionStart: enable quietly, and say so only when something changed or needs a decision."""
    top = worktree_top(Path.cwd())
    if top is None:
        return 0
    status, detail = enable(top)
    if status == "enabled":
        print(json.dumps({"systemMessage": "Enabled this worktree's git hooks: pre-commit checks formatting and document "
                                           "layout on what you commit (.githooks, scripts/install_hooks.py)."}))
    elif status == "conflict":
        print(json.dumps({"systemMessage": f"Git hooks not enabled: {detail}."}))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="report only; exit 1 if the hooks are not enabled")
    parser.add_argument("--claude-hook", action="store_true", help="Claude Code SessionStart")
    args = parser.parse_args(argv)
    if args.claude_hook:
        return claude_hook()
    top = worktree_top(Path.cwd())
    if top is None:
        print("install_hooks: not in a git worktree", file=sys.stderr)
        return 1
    status, detail = enable(top, check_only=args.check)
    print(f"{status}: {detail}")
    return 0 if status in ("enabled", "already") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
