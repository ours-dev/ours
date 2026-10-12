# high-res-timers: the one-shot tick, broadcast and replacement have landed behind a boot option; the review's main findings are fixed

**Date**: 2026-10-11

Where the work stands, for whoever picks it up. The design is `DESIGN.md`; each change is recorded in `docs/changes/261010/kernel-time.md`, with its numbers.

## Landed (branch `feat/high-res-timers`, worktree `.worktrees/feat-high-res-timers`)

| Commit | What |
|---|---|
| `7cf053cb` | `TimerQueue` stops recording a deadline the device refused |
| `d35b8771` | `Timer::forward()` moves a timer whose deadline is exactly now; `timer.test` |
| `81dec45b` | `on_tick()` runs the queue only in Legacy |
| `fece2d91` | `kernel.time.oneshot`, off by default, and the build argument `time_oneshot` |
| `2a158ef4` | a CPU runs one-shot when its own device is enrolled; the tick is a timer in its queue |
| `3740039a` | `timershare.test`, the measurement of the guard's case |
| `f7b2bfd3` | `timershare.test` compares each repeat with its own unloaded timing |
| `ee104cfc` | a tick device is swapped with interrupts off |
| `16af5198` | `TickSched` keeps whether the scheduler exists; the tick's source is the device's mode |
| `42aee901` | a global device serves several CPUs through a broadcast group; a CPU can move its tick between devices (`tickbcast.test`) |
| `b5a19149` | a CPU whose tick device refuses a deadline moves its tick to another device by itself (`tickreplace.test`) |
| `ce046d83`, `21ced28a` | the review of the branch (`REVIEW.md`); the scope of the HPET and the refusal policy in `DESIGN.md` |
| `01e8722a` | names: `ClockEventDevice::arm_or_park()`, `on_device_refused()`, `TickBroadcast::for_device()` |
| `84054d61` | review H1: a device that would not take the tick is marked failed first; `tickreplace.test` case three |
| `87e908bc` | review M1: a broadcast group leaves its handler on the device |
| `87ced9ea` | review M2: a device is asked three times before it is retired; `tickreplace.test` case four |
| `64a0d88e` | review M3: arming or cancelling a timer stays on one CPU through the notify (and closes an older window in `activate_timer()`) |
| the commit that adds this line | the review's smaller findings: orderings, the log order, an assertion, a comment |

Since 2026-10-11 the dev images boot one-shot by default; `time_oneshot=false` (`tools/set.py bringup.x86 --out out/periodic.x86 --args time_oneshot=false`) boots the periodic tick. In one-shot both images boot, every CPU ticks at 100 Hz, and a 1 ms timer is late by a median of about 0.9 ms instead of about 5 ms.

## Step 2: the guard against a callback slower than its period, measured and not written

`timershare.test` pins a thread to a CPU and measures the share of it that a 10 µs periodic timer leaves it (`docs/architecture/timer/callback-cpu-share.md` has the formula and the table). Under QEMU TCG the thread keeps 45 % to 54 % with callbacks of 63 to 150 µs, because the emulated local APIC timer delivers about 200 µs after it is armed and each interrupt costs about 110 µs outside its callback. The case the guard is for, interrupts back to back, cannot be produced here, and a guard cannot be checked here either. It is recorded as item 5 of `docs/repairs/261010/kernel-time.md`, to be revisited on hardware with a fast timer. Until then leave `time_oneshot` false in anything that ships.

The 110 µs and the 200 µs are item 4 of the same file: performance work, which goes through the user first.

## Open, with the user

- Nothing from the review is left open except what it called accepted: test-only counters in production headers, and the test helpers that are copied three times.

- Whether to write the guard anyway, untested, or to leave it to a machine with a fast timer (the recommendation).
- The performance numbers in the change record are a first measurement under TCG, not a study; any optimisation of the one-shot path goes through the user first.

## Worktree notes

The shared workspace has uncommitted work from other sessions (a `thread.cpp`); everything here was built and booted in the clean worktree. Landing on `baseline/20260824` is a cherry-pick of the commits above, in order.
