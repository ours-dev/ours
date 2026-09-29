#!/usr/bin/env python3
"""Check the layout of the Markdown this repo's agents write.

Many agents write documents here in turn, and left alone each invents its own
layout. This checks the mechanical part of the standard set out in the
"Formatting" section of CLAUDE.md, using the templates as the source of truth
for which fields and headings a document must have:

    docs/changes/TEMPLATE.md   fields of a change record entry
    docs/repairs/TEMPLATE.md   fields of a repair item
    .agents/tasks/TEMPLATE.md  headings of DESIGN, PLAN, RESULT and REVIEW

Usage:

    python3 scripts/lint_docs.py            # what you changed since HEAD
    python3 scripts/lint_docs.py PATH...    # whole files, every rule
    python3 scripts/lint_docs.py --all      # every tracked document
    python3 scripts/lint_docs.py --all --summary

The default mode reports only *new* violations, the way clang-tidy is used on
changed lines: an untracked or newly added file is checked whole, while a
modified file is checked only on the lines you added. Older documents are
Chinese and predate the standard, so a violation they already had is not yours
to fix. Exit status is 1 if anything is reported.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent

CHANGES_TEMPLATE = "docs/changes/TEMPLATE.md"
REPAIRS_TEMPLATE = "docs/repairs/TEMPLATE.md"
TASKS_TEMPLATE = ".agents/tasks/TEMPLATE.md"
TEMPLATES = {CHANGES_TEMPLATE, REPAIRS_TEMPLATE, TASKS_TEMPLATE}

# Third-party or generated Markdown that is not ours to reformat.
EXCLUDED_PREFIXES = (".agents/skills/skill-creator/",)

SLUG = r"[a-z0-9]+(?:-[a-z0-9]+)*"
CHANGES_PATH = re.compile(rf"^docs/changes/(\d{{6}})/({SLUG})\.md$")
REPAIRS_PATH = re.compile(rf"^docs/repairs/(\d{{6}})/({SLUG})\.md$")
DESIGN_PATH = re.compile(rf"^docs/design/(\d{{6}})/({SLUG})\.md$")
TASK_PATH = re.compile(rf"^\.agents/tasks/({SLUG})/([A-Z][A-Z0-9-]*)\.md$")

WRAP_COLUMNS = 80  # instruction files are read raw; documents are not hard-wrapped
FENCE = re.compile(r"^(\s*)(`{3,}|~{3,})(.*)$")
HEADING = re.compile(r"^(#{1,6}) +(.*?)\s*$")
BOLD_LABEL = re.compile(r"^\*\*(.+?)\*\*\s*$")
CODE_SPAN = re.compile(r"`[^`\n]*`")
URL = re.compile(r"https?://")
BAD_DATE = re.compile(r"\b\d{4}/\d{1,2}/\d{1,2}\b|\b\d{4}\.\d{2}\.\d{2}\b")
PLACEHOLDER = re.compile(r"YYYY-MM-DD|<MODULE>|<YYMMDD>|<topic>")
ENTRY_TITLE = re.compile(r"^## .+ \((\d{4}-\d{2}-\d{2})\)$")
SEVERITIES = ("High", "Medium", "Low")
STATUSES = ("Proposed", "Accepted", "Implemented", "Abandoned")


@dataclasses.dataclass(frozen=True)
class Violation:
    path: str
    line: int
    rule: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.message}"


# ---------------------------------------------------------------- templates


def fenced_blocks(text: str) -> list[list[str]]:
    """The lines of every fenced ```markdown block, in order."""
    blocks: list[list[str]] = []
    current: Optional[list[str]] = None
    for line in text.split("\n"):
        m = FENCE.match(line)
        if m and current is None and m.group(3).strip() == "markdown":
            current = []
        elif m and current is not None and m.group(3).strip() == "":
            blocks.append(current)
            current = None
        elif current is not None:
            current.append(line)
    return blocks


def entry_labels(template_text: str) -> list[str]:
    """Bold-only label lines of the template's skeleton: the required fields."""
    blocks = fenced_blocks(template_text)
    if not blocks:
        return []
    return [m.group(1) for line in blocks[0] if (m := BOLD_LABEL.match(line))]


def task_skeletons(template_text: str) -> dict[str, list[str]]:
    """For each `## KIND.md` section of the tasks template, the H2 headings of
    its skeleton, in order."""
    skeletons: dict[str, list[str]] = {}
    sections = re.split(r"^## ([A-Z][A-Z0-9-]*)\.md\s*$", template_text, flags=re.M)
    for kind, body in zip(sections[1::2], sections[2::2]):
        blocks = fenced_blocks(body)
        if blocks:
            skeletons[kind] = [m.group(1) for line in blocks[0] if (m := re.match(r"^## +(.*?)\s*$", line))]
    return skeletons


@dataclasses.dataclass
class Standard:
    change_labels: list[str]
    repair_labels: list[str]
    task_headings: dict[str, list[str]]

    @classmethod
    def load(cls, root: Path = ROOT) -> "Standard":
        def read(rel: str) -> str:
            p = root / rel
            return p.read_text(encoding="utf-8") if p.is_file() else ""

        return cls(entry_labels(read(CHANGES_TEMPLATE)), entry_labels(read(REPAIRS_TEMPLATE)),
                   task_skeletons(read(TASKS_TEMPLATE)))


# ------------------------------------------------------------------ parsing


@dataclasses.dataclass
class Parsed:
    lines: list[str]
    in_fence: list[bool]              # True for fence delimiters and everything between them
    open_fences: list[tuple[int, str]]  # (1-based line, info string)
    headings: list[tuple[int, int, str]]  # (1-based line, level, text), outside fences
    body_start: int                   # 0-based index of the first line after any front matter


def parse(text: str) -> Parsed:
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]  # the empty piece after the final newline is not a line
    body_start = 0
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                body_start = i + 1
                break
    in_fence = [False] * len(lines)
    open_fences: list[tuple[int, str]] = []
    headings: list[tuple[int, int, str]] = []
    fence: Optional[tuple[str, int]] = None  # (fence character, opening length)
    for i, line in enumerate(lines):
        m = FENCE.match(line)
        if fence is None:
            if m and i >= body_start:
                fence = (m.group(2)[0], len(m.group(2)))
                open_fences.append((i + 1, m.group(3).strip()))
                in_fence[i] = True
                continue
            h = HEADING.match(line) if i >= body_start else None
            if h:
                headings.append((i + 1, len(h.group(1)), h.group(2)))
        else:
            in_fence[i] = True
            if m and m.group(2)[0] == fence[0] and len(m.group(2)) >= fence[1] and m.group(3).strip() == "":
                fence = None
    return Parsed(lines, in_fence, open_fences, headings, body_start)


# ------------------------------------------------------------------- checks


def valid_date(text: str, fmt: str) -> bool:
    try:
        datetime.datetime.strptime(text, fmt)
        return True
    except ValueError:
        return False


def is_wrapped_file(path: str) -> bool:
    return path in ("CLAUDE.md", "AGENTS.md") or path.startswith(".agents/skills/")


def in_scope(path: str) -> bool:
    if not path.endswith(".md") or path.startswith(EXCLUDED_PREFIXES):
        return False
    return path in ("CLAUDE.md", "AGENTS.md") or path.startswith(("docs/", ".agents/"))


def sections(p: Parsed, level: int) -> list[tuple[int, int, str]]:
    """(first line, last line, title) of each heading of `level`, 1-based, the
    section running to the next heading of the same or a higher level."""
    result = []
    for idx, (line, lvl, text) in enumerate(p.headings):
        if lvl != level:
            continue
        end = len(p.lines)
        for later_line, later_lvl, _ in p.headings[idx + 1:]:
            if later_lvl <= level:
                end = later_line - 1
                break
        result.append((line, end, text))
    return result


def check_text(path: str, text: str, added: Optional[set[int]], std: Standard) -> list[Violation]:
    """Violations in one document. `added` holds the 1-based numbers of the lines
    to hold to the standard, or None when the whole file is new."""
    p = parse(text)
    is_new = added is None
    out: list[Violation] = []

    def wanted(line: int) -> bool:
        return is_new or line in added

    def add(line: int, rule: str, message: str) -> None:
        out.append(Violation(path, line, rule, message))

    wrapped = is_wrapped_file(path)
    for i, line in enumerate(p.lines):
        n = i + 1
        if not wanted(n) or p.in_fence[i]:
            continue
        if line != line.rstrip():
            add(n, "trailing-whitespace", "remove trailing whitespace")
        if "\t" in line:
            add(n, "tab", "use spaces, not tabs")
        if re.match(r"^\s*[*+] ", line) and not re.match(r"^\s*(\*\s*){3,}$", line):
            add(n, "bullet-marker", "bullet lists use '-'")
        prose = CODE_SPAN.sub("", line)
        if BAD_DATE.search(prose):
            add(n, "date-format", "write dates as YYYY-MM-DD")
        if (wrapped and i >= p.body_start and len(line) > WRAP_COLUMNS and line.rfind(" ", 0, WRAP_COLUMNS) > 0
                and not line.lstrip().startswith(("|", "#")) and not URL.search(line)):
            add(n, "line-length", f"wrap at {WRAP_COLUMNS} columns ({len(line)} here)")
    for n, info in p.open_fences:
        if wanted(n) and not info:
            add(n, "fence-language", "give the code block a language, 'text' for plain output")

    last_level = 0
    for n, level, _ in p.headings:
        if wanted(n) and last_level and level > last_level + 1:
            add(n, "heading-level", f"H{last_level} is followed by H{level}; do not skip a level")
        last_level = level

    if is_new:
        if text and not text.endswith("\n"):
            add(len(p.lines), "final-newline", "the file must end with a newline")
        elif text.endswith("\n\n"):
            add(len(p.lines), "final-newline", "the file must end with exactly one newline")
        h1s = [h for h in p.headings if h[1] == 1]
        if not h1s:
            add(1, "h1", "the document needs a title as its H1")
        else:
            if len(h1s) > 1:
                add(h1s[1][0], "h1", "only one H1 per document")
            first_content = next((i for i in range(p.body_start, len(p.lines)) if p.lines[i].strip()), None)
            if first_content is not None and first_content + 1 != h1s[0][0]:
                add(first_content + 1, "h1", "the H1 must come first")

    if path not in TEMPLATES:
        check_family(path, p, added, is_new, std, add, wanted)
    return out


def check_family(path, p, added, is_new, std, add, wanted) -> None:
    m_change, m_repair = CHANGES_PATH.match(path), REPAIRS_PATH.match(path)
    m_design, m_task = DESIGN_PATH.match(path), TASK_PATH.match(path)
    in_family = path.startswith(("docs/changes/", "docs/repairs/", "docs/design/", ".agents/tasks/"))
    if not in_family:
        return

    if is_new:
        expected = {"docs/changes/": "docs/changes/<YYMMDD>/<module>.md", "docs/repairs/": "docs/repairs/<YYMMDD>/<module>.md",
                    "docs/design/": "docs/design/<YYMMDD>/<topic>.md", ".agents/tasks/": ".agents/tasks/<topic>/<KIND>.md"}
        prefix = next(k for k in expected if path.startswith(k))
        if not (m_change or m_repair or m_design or m_task):
            add(1, "path", f"expected {expected[prefix]} (lowercase hyphenated names, KIND in capitals)")
        for m in (m_change, m_repair, m_design):
            if m and not valid_date(m.group(1), "%y%m%d"):
                add(1, "path", f"{m.group(1)} is not a YYMMDD date")

    # A placeholder left from a template means the skeleton was copied, not filled in.
    for i, line in enumerate(p.lines):
        n = i + 1
        if wanted(n) and not p.in_fence[i] and PLACEHOLDER.search(CODE_SPAN.sub("", line)):
            add(n, "placeholder", "template placeholder left in the text")

    h1 = next((h for h in p.headings if h[1] == 1), None)
    if is_new and h1:
        if m_change and h1[2] != f"{m_change.group(2)} changes":
            add(h1[0], "h1", f"the title of this file must be '# {m_change.group(2)} changes'")
        if m_repair and h1[2] != f"{m_repair.group(2)} outstanding repairs":
            add(h1[0], "h1", f"the title of this file must be '# {m_repair.group(2)} outstanding repairs'")
        topic = m_design.group(2) if m_design else (m_task.group(1) if m_task else None)
        if topic and not h1[2].startswith(f"{topic}: "):
            add(h1[0], "h1", f"the title must start with '{topic}: '")

    if m_change or m_repair:
        labels = std.change_labels if m_change else std.repair_labels
        for first, last, title in sections(p, 2):
            if not wanted(first):
                continue  # only an entry you added is held to the template
            if m_change and not ENTRY_TITLE.match(f"## {title}"):
                add(first, "entry-title", "entry titles end with the date: '## What changed (YYYY-MM-DD)'")
            if m_change and (dm := ENTRY_TITLE.match(f"## {title}")) and not valid_date(dm.group(1), "%Y-%m-%d"):
                add(first, "entry-title", f"{dm.group(1)} is not a date")
            if m_repair and not re.match(r"^\d+\. \S", title):
                add(first, "entry-title", "repair items are numbered: '## 1. What is wrong'")
            body = p.lines[first:last]
            present = [dm.group(1) for line in body if (dm := BOLD_LABEL.match(line))]
            missing = [label for label in labels if label not in present]
            if missing:
                add(first, "entry-fields", "missing " + ", ".join(f"**{x}**" for x in missing)
                    + " (a field with nothing to say gets 'None')")
            elif [x for x in present if x in labels] != [x for x in labels if x in present]:
                add(first, "entry-fields", "fields are out of order; follow the template")
            if m_repair and "Severity" in present:
                at = next(j for j, x in enumerate(body) if (dm := BOLD_LABEL.match(x)) and dm.group(1) == "Severity")
                value = next((x for x in body[at + 1:] if x.strip()), "")
                if not value.startswith(SEVERITIES):
                    add(first + at + 1, "severity", "the Severity text starts with High, Medium or Low")

    kind = m_task.group(2) if m_task else ("DESIGN" if m_design else None)
    if is_new and kind in std.task_headings:
        head = p.lines[p.body_start:p.body_start + 8]
        if not any(re.match(r"^\*\*Date\*\*: \d{4}-\d{2}-\d{2}$", x) for x in head):
            add(1, "header", "the first lines need '**Date**: YYYY-MM-DD'")
        if kind in ("DESIGN", "PLAN"):
            st = next((re.match(r"^\*\*Status\*\*: (.+)$", x) for x in head if x.startswith("**Status**")), None)
            if not st or st.group(1) not in STATUSES:
                add(1, "header", "the first lines need '**Status**: " + " | ".join(STATUSES) + "'")
        found = [t for _, _, t in sections(p, 2)]
        missing = [x for x in std.task_headings[kind] if x not in found]
        if missing:
            add(1, "sections", f"{kind}.md is missing: " + ", ".join(f"'## {x}'" for x in missing))
        elif [x for x in found if x in std.task_headings[kind]] != std.task_headings[kind]:
            add(1, "sections", f"the sections of {kind}.md are out of order; follow the template")


# ---------------------------------------------------------------------- git


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def added_lines(path: str) -> set[int]:
    lines: set[int] = set()
    for hunk in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", git("diff", "-U0", "HEAD", "--", path), re.M):
        start, count = int(hunk.group(1)), int(hunk.group(2) if hunk.group(2) is not None else 1)
        lines.update(range(start, start + count))
    return lines


def changed_files() -> dict[str, Optional[set[int]]]:
    """path -> the added line numbers, or None for a file that is new."""
    changed: dict[str, Optional[set[int]]] = {}
    for line in git("diff", "--name-status", "-M", "HEAD").splitlines():
        status, *names = line.split("\t")
        path = names[-1]
        if status.startswith("D"):
            continue
        changed[path] = None if status.startswith("A") else added_lines(path)
    for path in git("ls-files", "--others", "--exclude-standard").splitlines():
        changed[path] = None
    return {p: a for p, a in changed.items() if in_scope(p) and (ROOT / p).is_file()}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="check these whole files instead of your changes")
    parser.add_argument("--all", action="store_true", help="check every tracked document, whole")
    parser.add_argument("--summary", action="store_true", help="print counts per rule, not each violation")
    args = parser.parse_args(argv)

    targets: dict[str, Optional[set[int]]]
    if args.paths:
        targets = {str(Path(x).resolve().relative_to(ROOT)) if Path(x).is_absolute() else x: None for x in args.paths}
    elif args.all:
        targets = {p: None for p in git("ls-files").splitlines() if in_scope(p) and (ROOT / p).is_file()}
    else:
        targets = changed_files()

    std = Standard.load()
    violations: list[Violation] = []
    for path, added in sorted(targets.items()):
        text = (ROOT / path).read_text(encoding="utf-8", errors="replace")
        violations += check_text(path, text, added, std)

    if args.summary:
        counts = collections.Counter(v.rule for v in violations)
        for rule, n in counts.most_common():
            print(f"{n:>6}  {rule}")
        print(f"{len(violations):>6}  total in {len({v.path for v in violations})} of {len(targets)} files")
    else:
        for v in violations:
            print(v)
        if not violations:
            print(f"lint_docs: {len(targets)} document(s) checked, no violations")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
