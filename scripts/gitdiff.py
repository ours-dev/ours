"""Which lines of which files changed since HEAD.

The repo's checkers (format_cpp.py, lint_docs.py) hold you to the standard only
on lines you changed, because much of the tree predates the standard. That
needs the added line numbers of each file, which `git diff -U0` gives.

Two details make this more than a one-liner. Rename detection has to be on and
the diff has to cover the whole repository, not one pathspec: a file moved with
`git mv` and diffed by its new path alone looks entirely added, and every line
of its legacy body would be blamed on whoever touched it last. And a pure
deletion adds no lines, so it must not produce an empty range.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", re.M)


def git_env() -> dict[str, str]:
    """The environment for a git subprocess that has to find its repository from its own
    working directory. Git exports GIT_DIR to its hooks, with no GIT_WORK_TREE, and then
    `git rev-parse --show-toplevel` run from a subdirectory answers with that subdirectory
    instead of the top of the worktree: every path is computed against the wrong root and
    the prebuilt tools are not found. The pre-commit hook hit exactly this."""
    env = dict(os.environ)
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_PREFIX"):
        env.pop(name, None)
    return env


def has_head(top: Path) -> bool:
    """False in a repository with no commits yet, where there is nothing to diff against
    and every file is new."""
    return subprocess.run(["git", "rev-parse", "--verify", "-q", "HEAD"], cwd=top, capture_output=True, env=git_env()).returncode == 0


def repo_diff(top: Path) -> str:
    """Everything that differs from HEAD in the working tree, renames detected."""
    return subprocess.run(["git", "diff", "-U0", "-M", "--no-color", "HEAD"], cwd=top, capture_output=True, text=True, env=git_env()).stdout


def diff_section(diff: str, rel: str) -> str:
    """The part of a `git diff` that concerns `rel`, or '' if it has no changes."""
    for section in re.split(r"^(?=diff --git )", diff, flags=re.M):
        target = re.search(r"^\+\+\+ b/(.+)$", section, re.M) or re.search(r"^rename to (.+)$", section, re.M)
        if target and target.group(1) == rel:
            return section
    return ""


def changed_ranges(diff: str) -> list[tuple[int, int]]:
    """Added line ranges (first, last), 1-based, from `git diff -U0` output."""
    ranges = []
    for m in HUNK.finditer(diff):
        start, count = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        if count:  # a pure deletion adds no lines
            ranges.append((start, start + count - 1))
    return ranges


def added_lines(diff: str, rel: str) -> set[int]:
    lines: set[int] = set()
    for first, last in changed_ranges(diff_section(diff, rel)):
        lines.update(range(first, last + 1))
    return lines
