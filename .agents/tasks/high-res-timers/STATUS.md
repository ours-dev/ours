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
| `2a158ef4` | a CPU runs one-shot when its own device is enrolled; the tick is a timer in its queue |
| the commit that adds `timershare.test` | the measurement of the guard's case |

With `time_oneshot=true` (`tools/set.py bringup.x86 --out out/oneshot.x86 --args time_oneshot=true`) both images boot, every CPU ticks at 100 Hz, and a 1 ms timer is late by a median of about 0.9 ms instead of about 5 ms.

## Step 2: the guard against a callback slower than its period, measured and not written

`timershare.test` pins a thread to a CPU and measures the share of it that a 10 µs periodic timer leaves it (`docs/architecture/timer/callback-cpu-share.md` has the formula and the table). Under QEMU TCG the thread keeps 45 % to 54 % with callbacks of 63 to 150 µs, because the emulated local APIC timer delivers about 200 µs after it is armed and each interrupt costs about 110 µs outside its callback. The case the guard is for, interrupts back to back, cannot be produced here, and a guard cannot be checked here either. It is recorded as item 5 of `docs/repairs/261010/kernel-time.md`, to be revisited on hardware with a fast timer. Until then leave `time_oneshot` false in anything that ships.

The 110 µs and the 200 µs are item 4 of the same file: performance work, which goes through the user first.

## Open, with the user

- Whether to write the guard anyway, untested, or to leave it to a machine with a fast timer (the recommendation).
- The performance numbers in the change record are a first measurement under TCG, not a study; any optimisation of the one-shot path goes through the user first.

## Worktree notes

The shared workspace has uncommitted work from other sessions (a `thread.cpp`); everything here was built and booted in the clean worktree. Landing on `baseline/20260824` is a cherry-pick of the five commits.
