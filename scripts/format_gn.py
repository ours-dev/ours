#!/usr/bin/env python3
"""Format GN files with the repo's own gn, the whole file, keeping every list in its order.

    python3 scripts/format_gn.py PATH...          # format in place
    python3 scripts/format_gn.py --check PATH...  # report only; exit 1 if it would change
    python3 scripts/format_gn.py --claude-hook    # Claude Code PostToolUse payload on stdin

`gn format` normalises layout, and it also sorts lists such as sources and deps.
The sorting is not harmless here. Sorting sources changes the order of the object
files on the link line, and sorting deps changes the order of the -I directories,
and this tree has headers that shadow each other by include order (a generic
arch/ header and the x86 one behind the same prefix). Formatting every GN file
that way changed 153 of the 811 files a build produces, both dev kernel images
among them, so the sort is suppressed: a `# NOSORT` marker, which gn honours, is
put above every list assignment for the duration of the format and taken off
again. What is left is layout only.

That claim is checked on every file, not assumed: the tokens of the result, minus
comments and commas, must equal the tokens of the source. If they do not, the
file is left untouched and reported as unsafe. Whole-file rather than changed
lines because gn cannot format a range.

gn comes from prebuilt/gn/gn, the same binary the build uses, so the canonical
form does not depend on which gn happens to be on PATH. Set GN to use another.
A file that does not parse is reported as such and left untouched: after an edit
that is the useful thing to hear, because it means the edit broke the build.
The Claude Code hook (.claude/settings.json) runs this after every Edit and
Write; it never blocks an edit over its own failure.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

import claude_hook as claude_hook_helpers
from formatting import FormatError, common_skip_reason, repo_top, run

GN_SUFFIXES = {".gn", ".gni"}
GN_DOTFILE = ".gn"  # the root dotfile: GN syntax, and it has no suffix as far as pathlib is concerned


def is_gn_file(rel: str) -> bool:
    return Path(rel).suffix in GN_SUFFIXES or Path(rel).name == GN_DOTFILE


def find_gn(root: Path) -> str:
    override = os.environ.get("GN")
    if override:
        return override
    candidate = Path(root).absolute() / "prebuilt/gn/gn"  # absolute: gn runs with the repo as its cwd
    if os.access(candidate, os.X_OK):
        return str(candidate)
    found = shutil.which("gn")
    if found:
        return found
    raise FormatError("no gn: expected prebuilt/gn/gn (or set GN)")


# An assignment whose right-hand side starts a list literal: the statements gn may sort.
ASSIGNMENT = re.compile(r"^(?P<indent>\s*)[A-Za-z_]\w*\s*[+-]?=\s*\[")
NOSORT = "# NOSORT"  # gn honours exactly this comment, immediately above an assignment
# Strings first, so a '#' inside one is not taken for a comment; comments and commas are then dropped.
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|#[^\n]*|[A-Za-z_]\w*|\d+|[^\s\w"#]')


def tokens(text: str) -> list[str]:
    return [t for t in TOKEN.findall(text) if t != "," and not t.startswith("#")]


def gn_format(exe: str, top: Path, text: str) -> subprocess.CompletedProcess:
    try:
        return run([exe, "format", "--stdin"], cwd=top, check=False, input=text)
    except OSError as err:
        raise FormatError(f"cannot run {exe}: {err}")


def assignment_lines(lines: list[str]) -> list[int]:
    return [i for i, line in enumerate(lines) if ASSIGNMENT.match(line)]


def format_keeping_order(exe: str, top: Path, rel: str, before: str) -> tuple[str, str]:
    """(status, text): the file formatted with its lists left in the order they were written,
    where status is 'ok', 'invalid' (text is gn's error) or 'unsafe' (text says why)."""
    plain = gn_format(exe, Path(top), before)  # the layout gn would give, sorted lists and all
    if plain.returncode != 0:
        # A parse error exits non-zero and is printed on stdout, without a file name because the
        # source came in on stdin ("ERROR at :1:11: ..."): put the name back, as gn itself would.
        message = (plain.stdout or plain.stderr).strip() or "gn could not parse the file"
        return "invalid", message.replace("ERROR at :", f"ERROR at {rel}:", 1)

    # Every list assignment gets a marker, even one the author already marked: two in a row do no
    # harm, and taking one off again below leaves theirs where it was.
    out, k = [], 0
    for line in before.split("\n"):
        m = ASSIGNMENT.match(line)
        if m:
            out.append(f"{m.group('indent')}{NOSORT}")
            k += 1
        out.append(line)
    result = gn_format(exe, Path(top), "\n".join(out))
    if result.returncode != 0:
        return "unsafe", "gn rejected the source once the sort markers were added"

    formatted = []
    for line in result.stdout.split("\n"):
        if ASSIGNMENT.match(line) and formatted and formatted[-1].strip() == NOSORT:
            formatted.pop()  # the marker is ours: take it off again
        formatted.append(line)
    plain_lines = plain.stdout.split("\n")
    plain_at, kept_at = assignment_lines(plain_lines), assignment_lines(formatted)
    if not (k == len(plain_at) == len(kept_at)):
        return "unsafe", f"could not line up its {k} list assignments with gn's output ({len(kept_at)}, {len(plain_at)})"
    # A comment above an assignment makes gn put a blank line before it, so each marker added one
    # that plain gn would not. Give every assignment the blank line, or lack of one, that plain gn
    # gave it; going backwards keeps the indices valid as lines are removed.
    for index in reversed(range(k)):
        at, blank_before = kept_at[index], lambda ls, i: i > 0 and ls[i - 1].strip() == ""  # noqa: E731
        if blank_before(formatted, at) and not blank_before(plain_lines, plain_at[index]):
            del formatted[at - 1]
    after = "\n".join(formatted)
    if tokens(before) != tokens(after):
        return "unsafe", "the result differs from the source by more than layout"
    return "ok", after


def format_file(path: Path, check_only: bool = False, gn: Optional[str] = None) -> tuple[str, str]:
    """(status, detail): status is 'formatted', 'clean', 'invalid', 'unsafe' or 'skipped'."""
    path = path.resolve()
    top = repo_top(path)
    if top is None:
        return "skipped", "not in a git repository"
    try:
        rel = path.relative_to(top).as_posix()
    except ValueError:
        return "skipped", "outside the repository"
    if not is_gn_file(rel):
        return "skipped", "not a GN file"
    reason = common_skip_reason(top, rel)
    if reason:
        return "skipped", reason
    if not path.is_file():
        return "skipped", "no such file"

    exe = gn or find_gn(top)
    before = path.read_bytes()
    status, text = format_keeping_order(exe, top, rel, before.decode("utf-8", errors="replace"))
    if status != "ok":
        return status, text
    after = text.encode()
    if after == before:
        return "clean", "already formatted"
    if check_only:
        return "formatted", "would change"
    if path.read_bytes() != before:  # another agent edited it while we formatted: do not overwrite that
        return "skipped", "changed while formatting; will be formatted on its next edit"
    path.write_bytes(after)
    return "formatted", "reformatted the whole file"


def claude_hook(stdin_text: str) -> int:
    """PostToolUse: format the GN file the tool just wrote, or say why it does not parse."""
    path = claude_hook_helpers.edited_path(claude_hook_helpers.read_payload(stdin_text))
    if path is None:
        return 0
    status, detail = format_file(path)
    if status == "formatted":
        print(claude_hook_helpers.context(
            f"gn format {detail} in {path.name}. The file on disk changed, so Read it again before your next Edit to it."))
    elif status == "invalid":
        print(claude_hook_helpers.block(
            f"{path.name} no longer parses as GN, so the build is broken. Fix this before going on:\n{detail}"))
    elif status == "unsafe":
        print(claude_hook_helpers.context(
            f"gn format was not applied to {path.name}: {detail}. The file is unchanged; keep its layout consistent by hand."))
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
        needs_attention = False
        for p in args.paths:
            status, detail = format_file(Path(p), check_only=args.check)
            print(f"{p}: {status}: {detail}")
            needs_attention |= status == "invalid" or (status == "formatted" and args.check)
        return 1 if needs_attention else 0
    except FormatError as err:
        # Never block an edit over a formatter problem, but never hide it either. A hook exits 1
        # (shown, not blocking); on the command line 2 tells "could not check" from 1, "checked,
        # and it needs attention".
        print(f"format_gn: {err}", file=sys.stderr)
        return 1 if args.claude_hook else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
