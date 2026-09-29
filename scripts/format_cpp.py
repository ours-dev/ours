#!/usr/bin/env python3
"""Format the C and C++ lines you changed, with the repo's own clang-format.

    python3 scripts/format_cpp.py PATH...          # format in place
    python3 scripts/format_cpp.py --check PATH...  # report only; exit 1 if it would change
    python3 scripts/format_cpp.py --claude-hook    # Claude Code PostToolUse payload on stdin

Only the lines that differ from HEAD are formatted, the way `git clang-format`
does it, and a file that is new (not tracked yet) is formatted whole. That is
deliberate: about one C++ file in six in this tree is not clang-format clean,
mostly under system/lib, so formatting a whole existing file would bury a
three-line edit in unrelated churn. Files that are not this repo's to format
are left alone: vendored code, submodules, build output, the toolchain, and the
paths listed in .clang-format-ignore.

clang-format comes from the repo's prebuilt LLVM tree, the same one
cpp-write-and-verify names, so no PATH setup is needed. Set CLANG_FORMAT to use
another. The Claude Code hook (.claude/settings.json) runs this after every
Edit and Write, so an agent's C++ comes out formatted without anyone having to
remember to ask. It never blocks an edit: a failure is reported and the edit
stands.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}
# Never ours to format: generated, third-party, or another agent's checkout.
SKIP_PREFIXES = ("out/", "prebuilt/", "vendor/", ".legacy/", ".claude/", ".cache/")
HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", re.M)


class FormatError(Exception):
    pass


def run(args: list[str], cwd: Path, check: bool = True, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=check, **kw)


def find_clang_format(root: Path) -> str:
    override = os.environ.get("CLANG_FORMAT")
    if override:
        return override
    # Prefer the LLVM that also ships clang-tidy: it is the one the build compiles
    # with, and cpp-write-and-verify pairs the two.
    candidates = sorted(root.glob("prebuilt/clang/*/bin/clang-format"),
                        key=lambda p: ((p.parent / "clang-tidy").exists(), p.parts[-3]), reverse=True)
    for c in candidates:
        if os.access(c, os.X_OK):
            return str(c)
    found = shutil.which("clang-format")
    if found:
        return found
    raise FormatError("no clang-format: expected prebuilt/clang/*/bin/clang-format (or set CLANG_FORMAT)")


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


def ignored_by_file(top: Path, rel: str) -> bool:
    ignore = top / ".clang-format-ignore"
    if not ignore.is_file():
        return False
    for line in ignore.read_text(encoding="utf-8").splitlines():
        pattern = line.strip()
        if pattern and not pattern.startswith("#") and (rel == pattern or fnmatch.fnmatch(rel, pattern)):
            return True
    return False


def skip_reason(top: Path, rel: str) -> Optional[str]:
    if Path(rel).suffix not in SUFFIXES:
        return "not C/C++"
    if rel.startswith(SKIP_PREFIXES):
        return "generated or third-party"
    if any(rel.startswith(p) for p in submodule_paths(top)):
        return "inside a submodule"
    if ignored_by_file(top, rel):
        return "listed in .clang-format-ignore"
    return None


def changed_ranges(diff: str) -> list[tuple[int, int]]:
    """Added line ranges (first, last), 1-based, from `git diff -U0` output."""
    ranges = []
    for m in HUNK.finditer(diff):
        start, count = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        if count:  # a pure deletion adds no lines
            ranges.append((start, start + count - 1))
    return ranges


def diff_section(diff: str, rel: str) -> str:
    """The part of a `git diff` that concerns `rel`, or '' if it has no changes."""
    for section in re.split(r"^(?=diff --git )", diff, flags=re.M):
        target = re.search(r"^\+\+\+ b/(.+)$", section, re.M) or re.search(r"^rename to (.+)$", section, re.M)
        if target and target.group(1) == rel:
            return section
    return ""


def lines_to_format(top: Path, rel: str) -> Optional[list[tuple[int, int]]]:
    """Ranges to format, [] for nothing to do, None to format the whole file."""
    tracked = run(["git", "ls-files", "--error-unmatch", "--", rel], cwd=top, check=False).returncode == 0
    if not tracked:
        return None  # a new file: everything in it is yours
    # Rename detection on, and no pathspec: a `git mv`'d file diffed by its new path alone
    # looks entirely added, and its whole legacy body would be formatted as if it were yours.
    diff = run(["git", "diff", "-U0", "-M", "--no-color", "HEAD"], cwd=top, check=False).stdout
    return changed_ranges(diff_section(diff, rel))


def format_file(path: Path, check_only: bool = False, clang_format: Optional[str] = None) -> tuple[str, str]:
    """(status, detail): status is 'formatted', 'clean', or 'skipped'."""
    path = path.resolve()
    top = repo_top(path)
    if top is None:
        return "skipped", "not in a git repository"
    try:
        rel = path.relative_to(top).as_posix()
    except ValueError:
        return "skipped", "outside the repository"
    reason = skip_reason(top, rel)
    if reason:
        return "skipped", reason
    if not path.is_file():
        return "skipped", "no such file"
    ranges = lines_to_format(top, rel)
    if ranges == []:
        return "clean", "no changed lines"

    exe = clang_format or find_clang_format(top)
    args = [exe, "--style=file", "--fallback-style=none"]
    if ranges is not None:
        args += [f"--lines={a}:{b}" for a, b in ranges]
    before = path.read_bytes()
    result = run(args + [str(path)], cwd=top, check=False)  # to stdout: the file is not touched yet
    if result.returncode != 0:
        raise FormatError(f"clang-format failed on {rel}: {result.stderr.strip()[:300]}")
    after = result.stdout.encode()
    if after == before:
        return "clean", "already formatted"
    if check_only:
        return "formatted", "would change"
    # Write only if the file is still what we formatted: another agent editing the same
    # file in the gap would otherwise lose its change.
    if path.read_bytes() != before:
        return "skipped", "changed while formatting; will be formatted on its next edit"
    path.write_bytes(after)
    return "formatted", "reformatted " + ("the whole file" if ranges is None else "lines " + ", ".join(f"{a}-{b}" for a, b in ranges))


def claude_hook(stdin_text: str) -> int:
    """PostToolUse: format the file the tool just wrote, and tell the model."""
    try:
        payload = json.loads(stdin_text or "{}")
    except ValueError:
        return 0
    tool_input = payload.get("tool_input") or {}
    response = payload.get("tool_response") or {}
    target = tool_input.get("file_path") or (response.get("filePath") if isinstance(response, dict) else None)
    if not target:
        return 0
    path = Path(target)
    if not path.is_absolute():
        path = Path(payload.get("cwd") or os.getcwd()) / path
    status, detail = format_file(path)
    if status != "formatted":
        return 0
    note = (f"clang-format {detail} in {path.name} (the repo's .clang-format). The file on disk changed, "
            "so Read it again before your next Edit to it.")
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": note}}))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--check", action="store_true", help="report what would change, change nothing")
    parser.add_argument("--claude-hook", action="store_true", help="read a Claude Code PostToolUse payload on stdin")
    args = parser.parse_args(argv)
    try:
        if args.claude_hook:
            return claude_hook(sys.stdin.read())
        would_change = False
        for p in args.paths:
            status, detail = format_file(Path(p), check_only=args.check)
            print(f"{p}: {status}: {detail}")
            would_change |= status == "formatted" and args.check
        return 1 if would_change else 0
    except FormatError as err:
        # Never block an edit over a formatter problem, but never hide it either.
        print(f"format_cpp: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
