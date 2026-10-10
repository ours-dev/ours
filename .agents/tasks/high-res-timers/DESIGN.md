# high-res-timers: run software timers at their own deadlines instead of the 10 ms tick

**Date**: 2026-10-09
**Status**: Accepted (direction A, refactor R1, 2026-10-09)

## Problem
Every software timer fires at tick granularity: HZ is 100 (`kernel/include/ours/time/cfg.hpp:13`) and `TimerQueue::run_expired_timers()` runs only from the tick (`kernel/kernel/time/tick_sched.cpp:66`). A sleep or timeout is up to 10 ms late, and the sampler's shortest period, 10 µs, really samples every 10 ms (`docs/changes/261004/prof.md:66`). That is what forced it now: the sampler stress test of 2026-10-07 checks only a few dozen records per boot, because each round lasts less than a tick.

Nobody decided against high-resolution timers; they were never finished (research of 2026-10-09, four read-only agents):
- The pieces date from the first commit, 1387a1a3 (2026-04-16): `TickDevice::switch_to_oneshot()`, `TickSched` WorkMode::HpTick, the ClockSource `HpClock` flag, LAPIC TSC-deadline programming.
- The kernel/time repair of 4fbc0343 (2026-09-03) made Periodic `program_event()` a deliberate no-op and wrote the HpTick timer, and listed NOHZ, broadcast, slack and a clocksource watchdog as not built (`docs/repairs-20260830.md:2732`, `:3676`).
- No userspace call can ask for a finite deadline (`kernel/object/wait.cpp:68`, `kernel/object/port_dispatcher.cpp:124` take 0 or infinite), so nothing forced the work.

What exists and works today: the per-CPU LAPIC timer is one-shot only and already emulates the periodic tick by re-arming one tick ahead (`kernel/kernel/time/tick_device.cpp:228-240`); `ClockEventDevice::program_event()` with its min-delta clamp, the LAPIC count and deadline writers, the interrupt path into `handle_event()`, and `TimerQueue`'s reprogram after expiry.

What is missing:
- ~~No one-shot interrupt handler runs the timer queue; its only caller is `TickSched::on_tick()`.~~ Written on 2026-10-10 (`TickDevice::handle_oneshot_event()`, `docs/changes/261010/kernel-time.md`).
- ~~`switch_to_oneshot()` has no caller, is `protected`, and commits the mode before it knows the switch worked~~ Removed on 2026-10-10; a CPU enters one-shot when its own device is enrolled (`TickDevice::setup_oneshot_tick()`), as R1 says.
- ~~`on_tick()` runs the timer queue in every mode, so under HpTick the queue re-enters itself~~ Fixed on 2026-10-10 (`docs/changes/261010/kernel-time.md`); the device-interrupt entry that HpTick relies on is still to write.
- ~~`reprogram_locked()` caches a deadline the device refused, a lost wakeup in one-shot mode~~ Fixed on 2026-10-10 (`docs/changes/261010/kernel-time.md`); the test of the refusal path is `docs/repairs/261009/kernel-time.md` item 1.
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

Refactor R1, chosen by the user on 2026-10-09: one owner per fact, so the switch and the double writes stop being possible rather than being handled.
- Today each CPU has five pieces: `ClockEventDevice` (the hardware), `TickDevice` (binds a device, keeps its own Periodic/Oneshot mode, emulates periodic, advances jiffies), `TickSched` (keeps a second mode, Legacy/HpTick; `on_tick()` both consumes the tick and runs the queue), `TimerQueue` (programs the device through `TickDevice`) and `SchedClock` (programs the device directly). One fact, the mode, is kept twice, and one device has three writers: the periodic re-arm, `TimerQueue` and `SchedClock`.
- After R1, once a CPU's one-shot device (the LAPIC) is up, `TimerQueue` is the only writer of it. The device interrupt runs `TimerQueue::run_expired_timers()` and reprograms for the earliest deadline (Zircon `TimerQueue::Tick()`).
- The tick is a `Timer` in that queue from the moment the device is up, on that CPU. There is no switch of a running tick from periodic to one-shot, so `switch_to_oneshot()` as written goes. `TickSched::on_tick()` only consumes the tick: the lockup heartbeat, the scheduler tick, and jiffies on its CPU; it never runs the queue.
- `SchedClock::reset_preempt()` keeps its interface, so `fair.*` is untouched, but sets a preemption slot in `TimerQueue`, which arms the device for the earlier of the slot and the queue's head (Zircon `TimerQueue::PreemptReset()`, `UpdatePlatformTimerLocked()`).
- The periodic tick, emulated or real, serves only early boot: the global HPET or PIT tick before the LAPIC enrols, which the LAPIC calibration counts.

The steps are introduced to the user one at a time.

## Alternatives considered
- B, Linux hrtimers in full: clocksource watchdog, VALID_FOR_HRES, hang detection and min-delta growth, per-clock hrtimer bases. Rejected: about 5.8k lines in five files upstream, and most of it duplicates what `TimerQueue` already does.
- C, Zircon with no tick: the scheduler sets its own slice-end deadline in the timer queue (`TimerQueue::PreemptReset()`). Rejected for now: it requires changing the tick-driven scheduler, including `fair.*`.
- R0, wire one-shot into today's structure: add the handler and a caller for `switch_to_oneshot()`, and fix the latent bugs in place. Rejected: the mode stays recorded twice and the device keeps three writers, which is where most of the bugs found came from.
- R2, rewrite kernel/time after Zircon without a tick. Rejected for now, for the same reason as C.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Direction | A hybrid, B Linux hrtimers, C Zircon tickless | A | Not given | A, 2026-10-09 |
| How much of kernel/time to refactor on the way | R0 wire into today's structure; R1 `TimerQueue` the only writer, the tick a `Timer`; R2 Zircon rewrite without a tick | R1 | Not given | R1, 2026-10-09 |
| Record kernel/time's Linux-shaped parts in `ours-kernel-design` | Yes, as a hybrid; no | Yes | Not given | Yes, 2026-10-09 (`references/task.md`, Time) |
| Where each CPU enters one-shot | When its one-shot device comes up, on that CPU (follows from R1); from its own running tick (Linux); by IPI from one CPU | When its device comes up | Not given | Follows from R1, 2026-10-09 |
| The guard against a callback slower than its period | In the first step; in a later step | A later step | Not given | A later step (the next one), taken on 2026-10-10 after the recommendation was put and the user said to go ahead; not stated explicitly, so open to overruling |

## Testing and verification
- `ours-verify` with the boot option on and off: both images pass.
- `sleep.test`, `semaphore.test`, `waitqueue.test` on dbg (`scripts/kshell_run.py`): today they assert only "not before the deadline"; their logged elapsed times should drop from about 10 ms to near the request. A lateness histogram for 100 µs to 5 ms timers belongs in `benches/`.
- `lockup status`: about 100 ticks a second on every CPU.
- The sampler stress test: many more records per round.
- Cannot be verified here: the TSC and TSC-deadline paths (no TCG support).

## Open questions
- ~~Whether the jiffies counter should advance from the HpTick timer on its CPU~~ It does, by what `forward()` says went by (2026-10-10). The boot CPU's window before the Threading hook is not counted: `docs/repairs/261010/kernel-time.md` item 2.
- What the lateness numbers mean under TCG, where host scheduling dominates. Measured with `timer.test` on 2026-10-10: a 1 ms timer is late by a median of about 0.9 ms with the option on (about 5 ms off), and up to about 3 ms at worst; how much of the 0.9 ms is the host and how much the HPET reads and the interrupt path is not known.
- Implementation belongs in a session of its own for kernel/time.
