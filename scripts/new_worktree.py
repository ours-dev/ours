#!/usr/bin/env python3
"""Create a git worktree of this repository under .worktrees/, ready to build.

    python3 scripts/new_worktree.py <branch> [<start-point>]

Creates <branch> at <start-point> (default: HEAD) in
.worktrees/<branch with slashes as dashes>, or checks out an existing
<branch> there. Then:

- copies in the contents of the submodule directories. They are not
  initialised in git, and the main worktree keeps unversioned work in them,
  so a new worktree would otherwise get them empty;
- links prebuilt/ to the main worktree's, since toolchains are never fetched
  twice;
- turns the pre-commit hook on;
- configures out/bringup.x86 with tools/set.py, which runs its compiles
  through the compiler cache every worktree shares when
  prebuilt/ccache/ccache exists, so the first build here copies out what
  other worktrees already compiled instead of compiling it again.

Prints the new worktree's path. CLAUDE.md (Repo conventions) says why
worktrees live here and not under /tmp.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# The build directory configured up front: the one boot_check.py builds and
# boots when no --out is given.
DEFAULT_BUILD = "bringup.x86"


def git(*args: str, cwd: Path) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True,
                          text=True).stdout


def project_root(cwd: Path) -> Path:
    """The worktree new worktrees hang under: the one this runs in, or, when
    it runs inside one of the .worktrees/ themselves, the one those hang
    under. Not `git worktree list`'s first entry: that is the repository's
    original checkout, which may be another directory on another branch.
    """
    top = Path(git("rev-parse", "--show-toplevel", cwd=cwd).strip())
    if top.parent.name == ".worktrees":
        return top.parent.parent
    return top


def submodule_paths(main: Path) -> list[str]:
    try:
        out = git("config", "-f", ".gitmodules", "--get-regexp", r"submodule\..*\.path", cwd=main)
    except subprocess.CalledProcessError:
        return []
    return [line.split(maxsplit=1)[1] for line in out.splitlines()]


def main(argv: list[str]) -> int:
    if len(argv) not in (1, 2):
        print(__doc__)
        return 2
    branch = argv[0]
    start = argv[1] if len(argv) == 2 else "HEAD"

    main = project_root(Path.cwd())
    path = main / ".worktrees" / branch.replace("/", "-")
    if path.exists():
        print(f"{path} exists already", file=sys.stderr)
        return 1
    path.parent.mkdir(exist_ok=True)

    # The start point is read in the project root, so HEAD is its HEAD.
    exists = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}"],
                            cwd=main, capture_output=True).returncode == 0
    if exists:
        git("worktree", "add", str(path), branch, cwd=main)
    else:
        git("worktree", "add", "-b", branch, str(path), start, cwd=main)

    for sub in submodule_paths(main):
        source = main / sub
        # A submodule the main worktree no longer has, or has left empty, has
        # nothing to copy.
        if not source.is_dir() or not any(source.iterdir()):
            continue
        (path / sub).mkdir(parents=True, exist_ok=True)
        subprocess.run(["rsync", "-a", "--exclude", ".git", f"{source}/", f"{path / sub}/"],
                       check=True)

    (path / "prebuilt").symlink_to(main / "prebuilt")
    subprocess.run([sys.executable, "scripts/install_hooks.py"], cwd=path, check=True,
                   capture_output=True)

    # set.py's own notes (no compiler cache, no clang) go to stderr and are
    # passed on; its progress output is dropped, since this script's one line
    # of output is the new worktree's path. A failure leaves a worktree that
    # boot_check.py would configure itself, so it is reported, not fatal.
    conf = subprocess.run([sys.executable, "tools/set.py", DEFAULT_BUILD], cwd=path,
                          stdout=subprocess.DEVNULL)
    if conf.returncode != 0:
        print(f"note: tools/set.py {DEFAULT_BUILD} failed in {path}; "
              f"out/{DEFAULT_BUILD} is not configured", file=sys.stderr)
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
