# Agents

This file provides guidance to AI coding agents (Claude Code, Cursor, Codex,
Copilot, Antigravity, etc.) when working with code in this repository. Claude
Code reads CLAUDE.md instead of this file when both are present, so its
standing rules are mirrored there; keep the two in sync when you change one.

## Repository Overview

A microkernel modeled primarily on Zircon (Fuchsia's kernel), with a few
deliberate exceptions modeled on Linux instead (see `ours-kernel-design`),
built to explore the next generation of OS design. It embraces a
bleeding-edge stack — modern C++ (C++20/23), current LLVM, no legacy
compatibility constraints.

## Skills

If a task matches a skill, invoke it — follow its instructions exactly rather
than partially applying them or reimplementing what it already covers.

Skills live in `.agents/skills/<skill-name>/SKILL.md`. The ones nearly every
task needs:

- `ours-verify`: done means it builds and **both** images boot. Run it before
  every commit.
- `ours-kernel-design`: settled design decisions. Load it before reading code
  for any design, placement or review question about `kernel/` or `system/`.
- `cpp-write-and-verify`: every C++ edit, with `modern-cpp-development` for
  style.

## Repo conventions

- Tooling scripts are Python, never shell.
- Toolchains come from official upstream releases into `prebuilt/`. Never
  build the dev container or install anything into system directories.
- Lists describing build products are generated from GN metadata, not kept by
  hand. GN templates are named for their artifact (`rust_library`), not the
  tool.
- Benchmarks go in `benches/`; files that only register kshell console
  commands go in `kernel/commands`; new C++ libraries are scaffolded with
  copier from `tools/templates/cpp_library`.

## Workflow

Ask for missing details before starting only when a request is genuinely
underspecified or substantial enough to need a plan; proceed directly on a
small, well-specified, or mechanical change. Once a request needs a plan,
break it into subtasks, and give each one a dedicated workspace under
`.agents/tasks/` in the project root to hold its plan, track progress, and
record how it was validated and reviewed.

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
  put to the user.
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

## Documentation

**Change records** go in `docs/changes/<YYMMDD>/<MODULE>.md`, one section per
change, in a fixed format; the template and field descriptions are in
`docs/changes/TEMPLATE.md`. Write it the same round as the change, not after
the fact. This document is for learning, so "why this was a mistake" and
"which Linux/Zircon function it was fixed to match" need to be spelled out in
full — not just which line changed. Append-only.

**Outstanding repairs** go in `docs/repairs/<YYMMDD>/<MODULE>.md`, listing
only what is not yet fixed, ordered by severity, each item independently
claimable. Once an item is fixed, delete it from here and add an entry to
changes.

`<YYMMDD>` is a six-digit date, `<MODULE>` a hyphenated module path name
(`kernel-time`, `x86-clock`). Splitting by module is how the length is kept
under control — don't merge two things back into one ever-growing file.

`docs/repairs-20260830.md` is the single running log from before this
structure existed, a read-only archive; **several agents write to it
concurrently, so never rewrite the whole file**.

## Extension

### Creating a New Skill

Use the `skill-creator` skill (`.agents/skills/skill-creator/`): it drafts a
new skill, runs its eval suite, and can tune the description for reliable
triggering.
