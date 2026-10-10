# high-res-timers: step 1 has landed behind a boot option, the guard is next

**Date**: 2026-10-10

Where the work stands, for whoever picks it up. The design is `DESIGN.md`; each change is recorded in `docs/changes/261010/kernel-time.md`, with its numbers.

## Landed (branch `feat/high-res-timers`, worktree `.worktrees/feat-high-res-timers`)

| Commit | What |
|---|---|
| `7cf053cb` | `TimerQueue` stops recording a deadline the device refused |
| `d35b8771` | `Timer::forward()` moves a timer whose deadline is exactly now; `timer.test` |
| `81dec45b` | `on_tick()` runs the queue only in Legacy |
| `fece2d91` | `kernel.time.oneshot`, off by default, and the build argument `time_oneshot` |
| this one (it adds this file) | a CPU runs one-shot when its own device is enrolled; the tick is a timer in its queue |

With `time_oneshot=true` (`tools/set.py bringup.x86 --out out/oneshot.x86 --args time_oneshot=true`) both images boot, every CPU ticks at 100 Hz, and a 1 ms timer is late by a median of about 0.9 ms instead of about 5 ms.

## Next: the guard against a callback slower than its period

Not started; the user's go-ahead is needed for the background first (`CLAUDE.md`, "one step at a time"). The case: a timer whose callback takes longer than its period leaves its forwarded deadline in the past when it returns, so the queue programs the device at its minimum delta and the CPU is interrupted back to back. The sampler's 10 µs period with a kernel-stack unwind is one such timer; the tick is another if a tick costs more than 10 ms. Until it lands, leave `time_oneshot` false in anything that ships and do not profile below the callback's cost with the option on.

## Open, with the user

- Step 2 as described is the recommendation, not an explicit choice (`DESIGN.md`, Decisions).
- The performance numbers in the change record are a first measurement under TCG, not a study; any optimisation of the one-shot path goes through the user first.

## Worktree notes

The shared workspace has uncommitted work from other sessions (a `thread.cpp`); everything here was built and booted in the clean worktree. Landing on `baseline/20260824` is a cherry-pick of the five commits.
