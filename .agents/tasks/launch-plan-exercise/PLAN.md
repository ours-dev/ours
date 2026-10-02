# launch-plan-exercise: the user writes Plan::fill() by hand, and it is reviewed

**Date**: 2026-09-29
**Status**: Accepted

## Request
At each milestone the user writes one core function by hand, under a time limit, and the AI reviews it as a strict interviewer would (CLAUDE.md, "Close the learning loop"). For K23b the function is `launch::Plan::fill()` in `system/lib/launch/src/plan.cpp`: it turns a program's PT_LOAD segments into the mappings the launcher makes. The user asked to do the exercise in a session of its own, so that the main K23 session stays clean. This file is that session's brief.

Scope: `Plan::fill()` only. The class around it (`build()`, the accessors, `add_mapping()`, `add_copy()`, `set_private_size()`), the specification and the tests are done and are not to be changed by the AI during the exercise.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Which core function for K23b | `Plan::fill()`; K26's `port_wait` | `Plan::fill()` | None | `Plan::fill()`, 2026-09-29 |
| Shape of Plan | free function with an out-parameter; class with `build()` returning `ustl::Result` | class | None | class, 2026-09-29 (the user: prefer classes to C style; `std::array`, not C arrays) |
| How writable segments are backed | private VMO filled by an explicit Copy (the specification as it is); copy-on-write snapshot child of the file VMO, which needs `VmoChildOptions::SnapshotAtLeastOnWrite` in the kernel first | explicit Copy now, copy-on-write as a separate task | None | explicit Copy for this exercise, 2026-09-29; copy-on-write is a later task with its own design, after the kernel supports snapshot children |
| Who writes `Plan::make()` | the user under a time limit, reviewed afterwards; the AI | the user | None | the AI, 2026-09-29: the user's draft reached 13 of 28 tests and was reviewed, then the user asked for it to be written, saying a program-header parser teaches little; no row in `docs/authored.md`, and the change record says so |

## Steps
1. The user writes `Plan::fill()`. Time limit 45 minutes, counted from when they start. The user already has a partial draft in the file; it is theirs, and it stays. The AI gives no hints, writes no part of the function, and does not reveal how any test is passed. The AI may explain what a test's expectation means in terms of the specification when asked, and may point out a contradiction between a test and the specification if the user finds one (the AI then admits it and fixes the test).
2. When the user says they are done, or the time is up, the AI reviews the code as a strict interviewer: correctness first (run the tests and explain every failure), then edge cases (overflow, page rounding direction, pages shared between segments or between RELRO and data, input that is not trustworthy), then style against `modern-cpp-development`. Ask the user "why is it written this way" for at least two choices. Write the review in `.agents/tasks/launch-plan-exercise/REVIEW.md`.
3. The user fixes what they want to; the AI still writes none of `fill()`.
4. Commit: the class restructure (header, tests, `plan.cpp`) together with the user's `fill()`, with the user named as its author in the commit message. Add a row to `docs/authored.md`. Append an entry to `docs/changes/260929/launch.md` saying the free function became `Plan::build()` / `Plan::fill()` with `std::array` storage, and that `fill()` was written by the user; the earlier entry there still names `plan_mappings()`, and entries are append-only.

## Files
- `system/lib/launch/src/plan.cpp` — `Plan::fill()`, the user's; its specification is the comment above it
- `system/lib/launch/include/launch/plan.hpp` — the class, not to be changed during the exercise
- `system/lib/launch/test/plan_test.cpp` — 28 host tests, not to be changed unless the user shows one contradicts the specification
- `.agents/tasks/launch-plan-exercise/REVIEW.md` — the review, written in step 2
- `docs/authored.md`, `docs/changes/260929/launch.md` — step 4

## Test plan
Build and run the host tests:

```bash
prebuilt/ninja/ninja -C out/phys-host-tests && out/phys-host-tests/host/obj/system/lib/launch/test/plan
```

Done means all 28 pass. The tests were validated in the K23 session against a reference implementation that is not in the repository (28 of 28), and eight deliberate mistakes in that reference were each caught by at least two tests, so a failure is almost certainly in `fill()`. Nothing boots this code yet; `ours-verify` is only needed at the commit in step 4, to show nothing else broke.
