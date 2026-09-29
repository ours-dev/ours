# Task document templates

Each task gets its own directory, `.agents/tasks/<topic>/`, where `<topic>` is lowercase words joined by hyphens. Inside it, a document is one of the kinds below, named in capitals: `DESIGN.md`, `PLAN.md`, `RESULT.md`, `REVIEW.md`, and `STATUS.md` for a running progress log on a long task (free-form). Do not create both `review.md` and `REVIEW.md`.

Copy the skeleton for the kind you are writing and keep its headings and their order. A section with nothing to say gets the word "None", not a deleted heading, because a missing section reads as forgotten.

Every document starts with an H1 of the form `# <topic>: one line`, where `<topic>` is the directory name, then the `**Date**` line, then `**Status**` for DESIGN and PLAN.

A design that has been accepted and should outlive the task moves to `docs/design/<YYMMDD>/<topic>.md`, using the DESIGN skeleton.

Task documents written before this template are mostly in Chinese and use their own headings. They are not rewritten; new documents use the skeletons below.

## DESIGN.md

```markdown
# <topic>: one line saying what this designs

**Date**: YYYY-MM-DD
**Status**: Proposed

## Problem
What is missing or wrong, why it matters now, and what forced it: a userspace requirement, a failing boot, a measurement.

## Goals and non-goals
What this design must achieve, and what it deliberately does not try to.

## Design
The mechanism: objects, interfaces, states, ownership and locking. Name the Linux or Zircon counterpart down to the function. Use a timeline or a diagram when concurrency is involved.

## Alternatives considered
Each rejected option and the reason. A design without rejected options has not been thought through.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

Decisions that belong to the user: strategy, trade-offs, departures from a design doc or Zircon. "Decided" holds the user's choice and the date, and stays empty while the decision is open. Write "None" if there are no decisions.

## Testing and verification
How we will know it works: host tests, boot checks with `ours-verify`, measurements. Say what cannot be verified yet.

## Open questions
What is still unknown, one per bullet. Write "None" if nothing is open.
```

## PLAN.md

```markdown
# <topic>: one line saying what this plans

**Date**: YYYY-MM-DD
**Status**: Proposed

## Request
What was asked, in the user's terms, and where the scope ends.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

## Steps
Numbered. Each step says what it does, why now, how it is verified, and which commit lands it. One step per commit.

## Files
Files created, changed or deleted, one line each.

## Test plan
The tests and boot checks, with the expected output.
```

## RESULT.md

```markdown
# <topic>: one line saying what was delivered

**Date**: YYYY-MM-DD

## Delivered
What landed, with commit hashes.

## Verification
Build, tests, boot and measurements, with the numbers. Say "not measured" for anything that was not verified.

## Deviations from the design
Where the result differs from DESIGN.md or PLAN.md, and why. Write "None" if it does not.

## Found along the way
Problems noticed or fixed in passing. Unfixed ones go to `docs/repairs/`; give the path.

## Not done
What was deferred, and where it is recorded.
```

## REVIEW.md

```markdown
# <topic>: review of <scope>

**Date**: YYYY-MM-DD

## Scope and method
What was reviewed (files, directory, diff), which authorities and tools were used (`modern-cpp-development`, `ours-kernel-design`, clang-format, clang-tidy), and which findings were verified rather than read-only judgement.

## What is done well
Specific things worth keeping, each with `file:line`.

## High severity
### H1. Title
`file:line`. What is wrong, why it matters, and the fix.

## Medium severity
### M1. Title
`file:line`. As above.

## Minor and style
Bullets, naming fixes first.

## Teaching opportunities
### T1. Title
A concrete line, the mechanism behind it, and the Linux or Zircon counterpart. For a concurrency point: which invariant is protected, who accesses it concurrently, what the worst interleaving is, and what would change with a different lock.

## Suggested follow-up order
Numbered, naming fixes first.
```
