"""What the repo's formatters (format_cpp.py, format_gn.py) have in common: which
files are not this repo's to format, where the repository root is, and how a tool
that cannot run is reported.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Optional

from gitdiff import git_env

# Never ours to format: generated, third-party, or another agent's checkout.
SKIP_PREFIXES = ("out/", "prebuilt/", "vendor/", ".legacy/", ".claude/", ".cache/")


class FormatError(Exception):
    """A formatter could not do its job (no tool, or the tool failed): not a verdict on the file."""


def run(args: list[str], cwd: Path, check: bool = True, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=check, env=git_env(), **kw)


def repo_top(path: Path) -> Optional[Path]:
    try:
        out = run(["git", "rev-parse", "--show-toplevel"], cwd=path.parent).stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return None
    return Path(out).resolve() if out else None


def submodule_paths(top: Path) -> list[str]:
    gm = top / ".gitmodules"
    if not gm.is_file():
        return []
    out = run(["git", "config", "--file", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$"], cwd=top, check=False).stdout
    return [line.split(None, 1)[1].strip().rstrip("/") + "/" for line in out.splitlines() if " " in line]


def common_skip_reason(top: Path, rel: str) -> Optional[str]:
    """Why a file is not ours to format, whatever kind of file it is."""
    if rel.startswith(SKIP_PREFIXES):
        return "generated or third-party"
    if any(rel.startswith(p) for p in submodule_paths(top)):
        return "inside a submodule"
    return None
