# CLAUDE.md

This file gives Claude Code the standing rules for this repository. Claude
Code reads this file instead of AGENTS.md when both are present, so it
mirrors AGENTS.md's tool-agnostic sections in full and adds only what's
specific to Claude Code (the skill-discovery mechanics below). Keep the two
in sync when you change one.

## Repository Overview

A microkernel modeled primarily on Zircon (Fuchsia's kernel), with a few
deliberate exceptions modeled on Linux instead (see `ours-kernel-design`),
built to explore the next generation of OS design. It embraces a
bleeding-edge stack — modern C++ (C++20/23), current LLVM, no legacy
compatibility constraints.

## Skills

If a task matches a skill, invoke it — follow its instructions exactly rather
than partially applying them or reimplementing what it already covers.

Skills live in `.agents/skills/` so every agent can use them. Claude Code only
discovers the ones linked from `.claude/skills/`
(`ln -s ../../.agents/skills/<name> .claude/skills/<name>`). The ones nearly
every task needs:

- `ours-verify`: done means it builds and **both** images boot. Run it before
  every commit.
- `ours-kernel-design`: settled design decisions. Load it before reading code
  for any design, placement or review question about `kernel/` or `system/`.
- `cpp-write-and-verify`: every C++ edit, with `modern-cpp-development` for
  style.

## Repo conventions

- Tooling scripts are Python, never shell.
- C/C++ and GN you change are formatted for you: C/C++ on the lines you
  changed only, GN as a whole file with every list left in the order you wrote
  it. In Claude Code a hook does it (`.claude/settings.json`); elsewhere run
  `python3 scripts/format_cpp.py <path>` or `python3 scripts/format_gn.py
  <path>`. Never run `clang-format -i` on a whole existing file (a sixth of the
  tree is not clean, so it buries your change in churn), and never plain
  `gn format` (it sorts sources and deps, which reorders the link line and the
  include path and so changes what the build produces).
- Code comments say what a thing is and why, in this codebase's own terms.
  They do not name Zircon, Fuchsia or Linux: no "counterpart of", no "as
  Zircon does". Where a design follows another system, say so in the change
  record's Reference and in the design documents, never in the code.
- Toolchains come from official upstream releases into `prebuilt/`. Never
  build the dev container or install anything into system directories.
- Lists describing build products are generated from GN metadata, not kept by
  hand. GN templates are named for their artifact (`rust_library`), not the
  tool.
- Benchmarks go in `benches/`, even one that runs as a kshell command
  (`//benches/kernel:commands` links it); every metric of an in-kernel one
  says through `bench::Run` whether it needs real hardware, and is skipped
  under an emulator if it does. Files that only register kshell console
  commands go in `kernel/commands`; new C++ libraries are scaffolded with
  copier from `tools/templates/cpp_library`.
- A new git worktree goes under `.worktrees/<name>` in the project root,
  which git ignores: `python3 scripts/new_worktree.py <branch> [<start>]`
  makes one ready to build. Never
  under `/tmp` or a session scratchpad: the system clears those, and on
  2026-10-07 it took every scratch worktree with its build output, so the
  next change had to start from a full rebuild (the user's rule).

## Workflow

Ask for missing details before starting only when a request is genuinely
underspecified or substantial enough to need a plan; proceed directly on a
small, well-specified, or mechanical change. Once a request needs a plan,
break it into subtasks, and give each one a dedicated workspace under
`.agents/tasks/` in the project root to hold its plan, track progress, and
record how it was validated and reviewed, using the skeletons in
`.agents/tasks/TEMPLATE.md`.

## Commit after every change

One change, one commit, made as soon as that change builds and boots. Do not
let several fixes pile up in the working tree: once they blur together the
commit messages stop being writable, which is how this tree reached ~280
uncommitted paths.

The rhythm is: diagnose -> fix -> build and boot it (`ours-verify`) -> commit
-> next thing.
An update to the running repair document belongs in the same commit as the
change it describes.

This worktree often holds work from other concurrent sessions. Check
`git diff <file>` before staging: a file can carry both your hunks and someone
else's in-flight feature, and sweeping theirs into your commit is worse than
leaving yours uncommitted. Commit with `git commit -- <paths>`, never
`git add` followed by a bare `git commit`: the sessions share one index, and a
bare commit takes whatever the others have staged.

A pre-commit hook (`.githooks/pre-commit`) refuses a commit whose changed C/C++
lines are not clang-format clean, whose GN files are not in gn's layout, or
whose documents break the layout standard. Fix what it reports; do not bypass
it with `--no-verify`. It only reports, and only about what you changed. It is
switched on per worktree, on purpose: they share one `.git/config`, and a
branch that predates the hook must not start running the old one. A Claude Code
session turns it on by itself when it starts in a worktree that has the
scripts; anywhere else, run:

```bash
python3 scripts/install_hooks.py
```

## Working with the user

This project exists for the user's own learning, not to ship an impressive
system, so weigh a decision's teaching value alongside getting the task done
— never only the latter.

- **Ship features at "basically usable," not more.** Stop once it works and
  passes `ours-verify` — don't chase completeness, and don't add scope beyond
  what was asked.
- **Performance work comes after a feature is basically complete, and goes
  through the user in depth, item by item.** This is what the user values
  most here — they're building it into interview-ready experience. Before
  starting any performance work, lay out the measurement method, where the
  bottleneck is, and each candidate's mechanism and cost, and let the user
  decide; never optimize quietly, and never hand over a bare conclusion. A
  performance problem noticed while building a feature goes into
  `docs/repairs/`, not into an opportunistic fix. While building something to
  "basically usable," if a choice would cap its performance ceiling later
  (lock granularity, data structure, copy count), treat it as a decision to
  put to the user. Every performance item gets a report in `docs/perf/`: the
  raw samples of its baseline and of each optimisation stage, their
  statistics, and each stage's gain (the user's rule, 2026-10-25).
- **Look for the teaching angle everywhere, not only in concurrency.**
  Concurrency questions (which lock, what granularity, lock order, how much
  the lock covers, lock-free structures) are where it pays off most: use them
  to teach how to think about it — which invariant is being protected, who
  accesses it concurrently, what the worst interleaving looks like, and what
  would change with a different lock. The same applies to a code review:
  raise correctness bugs, design issues and robustness concerns together, not
  only the one thing asked about, because a narrow answer withholds input the
  user would have wanted — unless they've explicitly narrowed a follow-up and
  said they already know the rest, in which case answer only that.
- **Introduce a multi-step plan one step at a time, never as a roadmap.**
  Before starting each step, give its background — what makes it necessary
  now, why this approach — and teach any mechanism the user hasn't worked
  with before rather than naming the change in its own vocabulary. Build
  systems are where this comes up most (what ninja actually builds, what a
  kernel command-line switch does before any code reads it): an approval
  given before the mechanism is clear means nothing. Then stop and wait for
  their judgement — they may approve, redirect, or cancel the step — before
  doing any of it. Keep each introduction short.
- **Let data decide between locking options.** Write a benchmark in
  `benches/`, measure time held, contention rate, and latency distribution,
  and compare options with numbers, not just theory. Correctness is the
  exception: measurement can't prove "no race" — correctness rests on an
  invariant and lock-order argument, backed by lockdep and stress tests.
- **Close the learning loop: get output from the user, not just approval.**
  Picking a recommended option and saying "sounds good" exercises
  recognition, not the ability itself — an interview is a whiteboard with no
  AI. So:
  - **Teach first, then ask.** A prediction is only a fair test of what the
    user has been shown. Before asking for one, briefly teach the objects,
    states and mechanisms involved (what each piece is, which counters or
    locks exist, what the racing calls do), with a concrete timeline of who
    does what when; only then ask something answerable from that. A quiz on
    concepts the user never saw tests nothing and is unfair to them (they
    said so on 2026-09-28).
  - For an architecture or concurrency decision, have the user write a
    one-line prediction first (which option, why, what will go wrong),
    before giving the analysis. When it's something to measure, put the
    prediction in that change's change record and check it against the
    result once it's in. Skip this step for renames and mechanical changes.
  - For a concurrency decision (which lock, lock order, how much it covers),
    have the user write the argument first — which invariant it protects,
    who accesses it concurrently, what the worst interleaving is — then have
    the AI grade it. Don't skip this.
  - At each milestone, pick one core function for the user to write by hand:
    the AI gives the spec and a failing test, the user writes it under a
    time limit, and the AI reviews it as a strict interviewer would. Record
    functions the user wrote in `docs/authored.md`.
- **Strategy and decisions belong to the user.** When a strategy needs to be
  set, a trade-off made between approaches, or a design doc or Zircon needs
  to be departed from, stop first: lay out the background and details, give
  a few options you think are good with your recommendation marked, and wait
  for the user to decide.
- **Push back, don't flatter.** When the user's idea is wrong or one-sided,
  say so directly and explain why; if you have a better idea, raise it and
  argue for it.
- **Never do something quietly when unsure.** When something is hard or
  impossible to decide on your own, write it up as a question for the user
  instead of deciding on your own and implementing it silently.
- **Keep answers short.** A few sentences or a short list, leading with the
  conclusion rather than the reasoning behind it, and skipping section
  headings, restated questions, and summaries of what was just said. Prefer
  one recommended option over a survey of alternatives — the strategy bullet
  above is the deliberate exception — and leave out background the user
  already knows. Depth is welcome on request: offer to expand one point in a
  short line and let them ask, rather than writing the expansion up front.
- **Explain a mechanism as objects, then events.** When asked what a scenario,
  flow or mechanism is, first list the objects involved, one line each saying
  what it is; then list the events that happen to them, in order, one line
  each. Do not open with a metaphor or a long timeline, and do not follow the
  first example with a fuller one. "I did not follow" or "too complicated"
  means the answer was too long or too dense: shorten it into those two lists,
  never add detail (the user said so three times in a row on 2026-09-29, about
  a copy-on-write design).

## Documentation

**Change records** go in `docs/changes/<YYMMDD>/<MODULE>.md`, one section per
change, in a fixed format; the template and field descriptions are in
`docs/changes/TEMPLATE.md`. Write it the same round as the change, not after
the fact. This document is for learning, so "why this was a mistake" and
"which Linux/Zircon function it was fixed to match" need to be spelled out in
full — not just which line changed. Append-only.

**Outstanding repairs** go in `docs/repairs/<YYMMDD>/<MODULE>.md` (template:
`docs/repairs/TEMPLATE.md`), listing only what is not yet fixed, ordered by
severity, each item independently claimable. Once an item is fixed, delete it
from here and add an entry to changes.

**Performance reports** go in `docs/perf/<YYMMDD>/<topic>.md` (template:
`docs/perf/TEMPLATE.md`), one per performance topic, with its raw samples
beside it in `docs/perf/<YYMMDD>/<topic>/`, one CSV file per stage. Every
statistic in a report comes from those samples through
`scripts/perf_stats.py`, never by hand, and each optimisation stage records
its gain over the baseline and over the stage before.

`<YYMMDD>` is a six-digit date, `<MODULE>` a hyphenated module path name
(`kernel-time`, `x86-clock`). Splitting by module is how the length is kept
under control — don't merge two things back into one ever-growing file.

`docs/repairs-20260830.md` is the single running log from before this
structure existed, a read-only archive; **several agents write to it
concurrently, so never rewrite the whole file**.

## Formatting

Many agents write documents here in turn, so a document's shape must not
depend on who wrote it: copy the template instead of inventing a layout.
`scripts/lint_docs.py` checks what you changed, so an older document's habits
are not yours to fix. Claude Code runs it after every edit of a document, and
the pre-commit hook runs it again on what you commit; elsewhere run
`python3 scripts/lint_docs.py --changed <path>`.

- Change record: `docs/changes/<YYMMDD>/<MODULE>.md`, template
  `docs/changes/TEMPLATE.md`.
- Repair item: `docs/repairs/<YYMMDD>/<MODULE>.md`, template
  `docs/repairs/TEMPLATE.md`.
- Performance report: `docs/perf/<YYMMDD>/<topic>.md`, template
  `docs/perf/TEMPLATE.md`.
- Task design, plan, result and review: `.agents/tasks/<topic>/DESIGN.md`,
  `PLAN.md`, `RESULT.md`, `REVIEW.md`, skeletons in `.agents/tasks/TEMPLATE.md`.
  An accepted design moves to `docs/design/<YYMMDD>/<topic>.md`.
- Everything is written in English: documents, skills, code, comments and
  commit messages. Older documents are in Chinese and are not rewritten; when
  you add to one, write the new part in English.
- Keep a template's headings and their order. A section with nothing to say
  gets the word "None", not a deleted heading.
- Markdown: `-` for bullets, one H1 that comes first, no skipped heading
  levels, a language on every code fence (`text` for plain output), dates as
  `YYYY-MM-DD`, no trailing whitespace or tabs, and a final newline.
- Documents are not hard-wrapped: one paragraph or list item per line.
  CLAUDE.md, AGENTS.md and skills are read raw, so they wrap at 80 columns.
- Names are lowercase words joined by hyphens; the kind of a task document
  (DESIGN, PLAN, RESULT, REVIEW, STATUS) is in capitals.
- Commit messages are `type(scope): subject`, with the types listed in
  `.gitmessage`.
