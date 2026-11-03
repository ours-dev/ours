---
name: cpp-code-review
description: Review C++ in this repo -- a file, a directory, a diff or a design -- and write the findings up as a report. Use when the user asks to review, audit or critique C++ code ("review 一下", "帮我看看这段代码", "这个设计有什么问题", "审一下 kernel/lib/xxx"), and for a self-review of your own change before committing. Reports mistakes first (undefined behavior, concurrency), then drift from local conventions, then teaching opportunities for the user's C++ and OS skills.
---

# C++ code review

## When to use this skill

Use this skill when asked to review C++ code in this repository: a file, a
directory, a diff, or a design. It says how to review; the authorities on what
is right are `modern-cpp-development` (style and language rules) and, for
anything under `kernel/` or `system/`, `kernel-design` (settled design
decisions). Load both before reading the code under review.

## Persona

Assume the role of an expert in low-level C++ for operating systems: friendly,
but direct. The user reads reviews to learn, so name real problems plainly
instead of reassuring them.

## Priorities

Review from several angles at once; a narrow review withholds input the user
would have wanted. You are looking for:

1. Ways the code is wrong.
1. Ways it drifts from the surrounding code and the repo's conventions.
1. Teaching opportunities for the user's command of C++ and of operating
   systems.

### 1. Looking for mistakes

The most critical mistakes are those which could introduce Undefined Behavior
(UB). Look for those first. Common sources of UB include:

- Signed integer overflow
- Null pointer dereference
- Out of bounds access (buffer overflow)
- Use after free (lifetime issues)
- Data races
- Uninitialized variables

Carefully examine the lifecycle of objects and whether references or pointers to
them can outlive the objects themselves.

Look for places where nullability is not correctly checked.

Be on the lookout for concurrency mistakes. Scrutinize atomic operations very
closely. Identify desired synchronization properties and ensure sync points are
correctly paired with acquire & release orderings on the right sides of
operations. Pay extra attention to relaxed or sequentially-consistent orderings,
they can be a sign of underbaked concurrency abstractions.

Once you've identified or ruled out causes of UB, look for missing error
handling, logical invariant violations, and the like.

Where `kernel-design` says a design follows Zircon or Linux, compare
against that counterpart (the local Fuchsia checkout, or Linux) and name the
function you compared. A divergence is a finding only if it is not one of the
documented ones.

### 2. Matching local style & conventions

The authorities are `modern-cpp-development`, `.clang-format`, `.clang-tidy`
and the surrounding files. Pay extra attention to this in your suggestions for
modifications to the user's code.

Identifying ways that local conventions could be improved or modernized is still
great information for your review comments, as these are excellent things for
the user to learn.

### 3. Teaching opportunities

If applicable, generate recommendations about idiomatic and defensive C++
programming to help the user improve their understanding of the language.

For example, if they've made a mistake with pointer lifetimes, don't just tell
them they have a source of undefined behavior. Illustrate the sequence of
operations and their relative timing that could lead to a use-after-free or
similar issue, and include references to websites like cppreference.com to help
the user understand the issue.

Tie each point to a concrete line, and name the Linux or Zircon counterpart
where there is one. For a locking or concurrency point, teach it the way
CLAUDE.md asks: which invariant is being protected, who accesses it
concurrently, what the worst interleaving is, and what would change with a
different lock. A few well-worked points beat a long list.

## Gotchas

- Several things that look like defects are deliberate, so check
  `kernel-design` before flagging: empty or stub classes, functions and
  init steps (scaffolding for planned work: ask about them, never recommend
  deleting them), `WEAK_LINK` generic defaults, `Fair` not being built by
  default (the user's own scheduler: never suggest editing it; record a bug
  in it for `docs/repairs` instead). An earlier review of `kernel/lib/arch`
  put "delete the empty stubs" first; that is a question for the user here,
  not a finding.
- When several cleanups are on the table, list naming fixes first: the user
  values them most, and later edits then land on the right names.
- Review, don't fix. The deliverable is the report; changing the code under
  review is a separate step the user picks.

## Process

### Read the authorities

Read `modern-cpp-development`, `kernel-design` (its references for the
subsystem in question), `.clang-format`, `.clang-tidy`, and the files
surrounding whatever is under review.

### Read the provided code

If the user has provided you with source references or a diff, refer to that.
If no scope was given, ask what to review.

If you're reviewing a diff, open the full files to understand more of what's
happening with the change.

### Back the findings with evidence

Run the repo's own `clang-format --dry-run -Werror` and `clang-tidy` on the
files under review, with the commands from `cpp-write-and-verify` (the
prebuilt LLVM tree, on the host). When a finding is about runtime behavior,
reproduce it with `verify` or a host test where that is practical. In the
report, say which findings were verified and which are read-only judgement.

### Produce a report

Write the review to `.agents/tasks/<topic>/REVIEW.md`, creating the task
directory if needed, from the REVIEW.md skeleton in
`.agents/tasks/TEMPLATE.md`, and run `python3 scripts/lint_docs.py` on it
before you finish.

Do not paste the report into the conversation. Reply with the path and the
three to five findings that matter most.
