# high-res-timers: run software timers at their own deadlines instead of the 10 ms tick

**Date**: 2026-10-09
**Status**: Proposed

## Problem
Every software timer fires at tick granularity: HZ is 100 (`kernel/include/ours/time/cfg.hpp:13`) and `TimerQueue::run_expired_timers()` runs only from the tick (`kernel/kernel/time/tick_sched.cpp:66`). A sleep or timeout is up to 10 ms late, and the sampler's shortest period, 10 µs, really samples every 10 ms (`docs/changes/261004/prof.md:66`). That is what forced it now: the sampler stress test of 2026-10-07 checks only a few dozen records per boot, because each round lasts less than a tick.

Nobody decided against high-resolution timers; they were never finished (research of 2026-10-09, four read-only agents):
- The pieces date from the first commit, 1387a1a3 (2026-04-16): `TickDevice::switch_to_oneshot()`, `TickSched` WorkMode::HpTick, the ClockSource `HpClock` flag, LAPIC TSC-deadline programming.
- The kernel/time repair of 4fbc0343 (2026-09-03) made Periodic `program_event()` a deliberate no-op and wrote the HpTick timer, and listed NOHZ, broadcast, slack and a clocksource watchdog as not built (`docs/repairs-20260830.md:2732`, `:3676`).
- No userspace call can ask for a finite deadline (`kernel/object/wait.cpp:68`, `kernel/object/port_dispatcher.cpp:124` take 0 or infinite), so nothing forced the work.

What exists and works today: the per-CPU LAPIC timer is one-shot only and already emulates the periodic tick by re-arming one tick ahead (`kernel/kernel/time/tick_device.cpp:228-240`); `ClockEventDevice::program_event()` with its min-delta clamp, the LAPIC count and deadline writers, the interrupt path into `handle_event()`, and `TimerQueue`'s reprogram after expiry.

What is missing:
- No one-shot interrupt handler runs the timer queue; its only caller is `TickSched::on_tick()`.
- `switch_to_oneshot()` has no caller, is `protected` (`kernel/include/ours/time/tick_device.hpp:111`), and commits the mode before it knows the switch worked (`docs/repairs/261009/kernel-time.md` item 3).
- `on_tick()` runs the timer queue in every mode, so under HpTick the queue re-enters itself (`docs/repairs/260926/kernel-time.md` item 2).
- `reprogram_locked()` caches a deadline the device refused, a lost wakeup in one-shot mode (`docs/repairs/261009/kernel-time.md` item 1).
- No guard against a timer whose callback costs more than its period, which in one-shot mode fires back to back.
- No clock source sets `HpClock`, so `select_best_source()` finds none once a CPU is one-shot (`docs/repairs/260903/kernel-time.md` item 2); there is no TSC clock source (`docs/repairs/260903/x86-clock.md` item 3).
- `SchedClock::reset_preempt()` programs the device behind the timer queue's back (`docs/repairs/261009/kernel-time.md` item 2), latent while `sched_nohz` is off.

Hardware under our QEMU (11.1.1, TCG, `-M q35 -cpu Nehalem`, `scripts/qemu_boot.py:223`): LAPIC count mode at 1 GHz and HPET at 100 MHz are available; TSC-deadline and invariant TSC are not supported by TCG at all, so the TSC paths cannot be tested here. Delivery precision is bounded by host scheduling (µs to ms), whatever the guest programs.

## Goals and non-goals
Goals:
- A software timer fires near its own deadline, not at the next tick: sleeps, timeouts and the sampler's period become real.
- The scheduler keeps its 100 Hz tick, unchanged; `kernel/kernel/task/sched/fair.*` is not touched.
- The lockup detector keeps receiving its per-CPU heartbeat.
- One-shot mode can be turned off by a boot option while it settles.

Non-goals, for now:
- Stopping the tick in idle (NOHZ), and a tickless scheduler.
- A TSC clock source and TSC-deadline programming, which TCG cannot exercise.
- Timer slack and coalescing.

## Design
Direction A, chosen by the user on 2026-10-09: a hybrid.
- The mode switch follows Linux, whose shape the code already has: each CPU moves its own tick device from periodic to one-shot, with interrupts off, and the tick survives as a timer (Linux `tick_check_oneshot_change()` -> `hrtimer_switch_to_hres()` -> `tick_switch_to_oneshot()`, `tick_setup_sched_timer()`).
- The timer engine follows Zircon: the existing per-CPU `TimerQueue` is the only one, run from the device interrupt and reprogrammed for its earliest deadline (Zircon `TimerQueue::Tick()` and `UpdatePlatformTimer()` in `zircon/kernel/kernel/timer.cc`). No Linux hrtimer bases, timer wheel or softirq.
- The tick is kept, as the HpTick timer, because the scheduler is tick-driven.
- Later, the clock layer follows Zircon's policy: trust the TSC only when CPUID reports it invariant, and program TSC-deadline with an absolute TSC value (Zircon `pc_init_timer()`, `platform_set_oneshot_timer()` in `zircon/kernel/platform/pc/timer.cc`).

How far the existing time code is refactored on the way is an open decision (see Decisions). The steps are introduced to the user one at a time.

## Alternatives considered
- B, Linux hrtimers in full: clocksource watchdog, VALID_FOR_HRES, hang detection and min-delta growth, per-clock hrtimer bases. Rejected: about 5.8k lines in five files upstream, and most of it duplicates what `TimerQueue` already does.
- C, Zircon with no tick: the scheduler sets its own slice-end deadline in the timer queue (`TimerQueue::PreemptReset()`). Rejected for now: it requires changing the tick-driven scheduler, including `fair.*`.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Direction | A hybrid, B Linux hrtimers, C Zircon tickless | A | Not given | A, 2026-10-09 |
| How much of kernel/time to refactor on the way | Open; options to be put to the user | Open | Not given | |
| Record kernel/time's Linux-shaped parts in `ours-kernel-design` | Yes, as a hybrid; no | Yes | Not given | |
| Where each CPU switches to one-shot | From its own tick once Threading is reached (Linux); by IPI from one CPU | From its own tick | Not given | |
| The guard against a callback slower than its period | In the first step; in a later step | Open | Not given | |

## Testing and verification
- `ours-verify` with the boot option on and off: both images pass.
- `sleep.test`, `semaphore.test`, `waitqueue.test` on dbg (`scripts/kshell_run.py`): today they assert only "not before the deadline"; their logged elapsed times should drop from about 10 ms to near the request. A lateness histogram for 100 µs to 5 ms timers belongs in `benches/`.
- `lockup status`: about 100 ticks a second on every CPU.
- The sampler stress test: many more records per round.
- Cannot be verified here: the TSC and TSC-deadline paths (no TCG support).

## Open questions
- Whether the jiffies counter (`ClockSource::Default`, advanced only from `handle_periodic_event()`) should advance from the HpTick timer on its CPU, or stay frozen as it is harmless today.
- What the lateness numbers mean under TCG, where host scheduling dominates.
- Implementation belongs in a session of its own for kernel/time.
