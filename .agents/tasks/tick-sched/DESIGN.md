# tick-sched: TickSched as the scheduler's tick

**Date**: 2026-11-06
**Status**: Proposed

## Problem
`TickSched` was created to be the scheduler's tick (the user, 2026-11-06) and was implemented roughly. Today it is a flag that says the scheduler exists and an `on_tick()` that runs the timer queue, called only when a CPU's tick device is Periodic. The default images boot every CPU one-shot, where a CPU has no tick at all; the discipline hook `on_thread_tick` has no caller, and `Scheduler::Current::tick()` was deleted (c6764d82). So the scheduler has nowhere to hang periodic work. The first piece of such work is the balance of busy CPUs against each other (repair item 22; `.agents/tasks/periodic-balance/`), which three forms of a bespoke trigger (a worker thread, a timer per CPU, the slice end) were each a compromise for. The user wants `TickSched` to work as intended instead.

How it came to be what it is (read by the research of 2026-11-06, commits cited): the first version (1387a1a3) was a self-re-arming tick `Timer` on every CPU with a `WorkMode`, `m_last_tick`, `m_next_tick`; 7eded2a4 dropped `WorkMode` as a second copy of the device's mode; the lockup heartbeat and the profiler moved to timers of their own (c61f59a5, 8e7557f1); c6764d82 moved the end of a slice to the preemption deadline, a slot in the CPU's `TimerQueue`; 1ede96ab removed the tick `Timer`, which fired every 10 ms on every CPU, idle ones included, taking an idle CPU from about 101 timer interrupts a second to about one; c65a75cc removed the two unused fields. That was the idle-power work, and decision (ii) of 2026-10-13 ("No tick drives the scheduler: Zircon's tickless model", chosen against the recommended Linux NOHZ idle) is written into `references/task.md`.

## Goals and non-goals
Goals:
- `TickSched` owns the tick: a periodic tick of the scheduler at `HZ`, on a CPU while it is busy, none while it is idle, with one scheduler function (`Scheduler::Current::tick(now)`) that the tick calls and from which periodic scheduler work hangs. The same function is called in both device modes.
- Nothing is lost of what idle-power won: an idle CPU takes about one interrupt a second; a busy CPU takes none more than today unless something wants the tick.
- The first consumer is the periodic balance (the knob `balance-period-ms`, off by default).
- The tick is counted and reported, and the tests that pin interrupt counts cover it.

Non-goals: the end of a slice (it stays the preemption deadline); jiffies (they stay, standing still in one-shot: the user's call of 2026-10-23); calling the discipline's `on_thread_tick` (round-robin's is empty, Fair's has never run and Fair is the user's: repair item 8); timekeeping, RCU and full-NO_HZ duties, which Linux's `tick_sched` carries and this kernel does not need; a nohz-style kick of halted CPUs (repair item 23, a later consumer); stopping the tick of a Periodic CPU while it idles (below); renaming or folding `TickSched`.

## Design
**Facts the design rests on** (research of 2026-11-06, `file:line` from the three reports; the slot mechanism below was re-read by hand):
- `TimerQueue::reprogram_locked()` is the only writer of a CPU's one-shot device; it arms for `earliest_deadline_locked()` = the earliest of the timers (held off after a slow pass) and `m_preemption_deadline` (never held off) (`timer_queue.cpp:72-123`).
- `Scheduler::set_next_thread()` arms the preemption deadline at every pick of a non-idle thread, the running thread picked again included, and clears it for the idle thread, under the run queue lock; that lock comes before the queue's. A refusal of the device there is not passed on; it is a self timer-IPI, and the queue passes it on with no lock held (`scheduler_inline.hpp:41-65`, `timer_queue.cpp:171-195`).
- `Timer::activate()` and `cancel()` call `notify_refusal()`, which may not run under the run queue lock, so a `Timer` cannot be armed from the pick (`timer_queue.cpp:182-191, 332`).
- The idle/busy edge that exists everywhere is the pick. The restart cannot be an interrupt: with MWAIT a waker writes the monitor word and no interrupt happens, and `come_back()` enters the idle loop without picking (`idle_thread.cpp`, `arch/x86/kernel/task/idle_thread.cpp:155-162`).
- A one-shot CPU has no tick; a busy one is interrupted by its timers, its 1 Hz lockup heartbeat and its slice end (about 15 a second); an idle one about once a second. A Periodic CPU's device fires every `1/HZ` busy or idle, and `program_event()` is a no-op there (`tick_device.cpp:56-76`). `idle.test` allows 5 timer interrupts per CPU per second (`MAX_QUIET_INTERRUPTS`).
- `take_over_from()` moves every queued `Timer` of a CPU going offline to the survivor; a deadline slot is not a `Timer` and is not moved. A CPU goes offline from its idle thread, whose pick has emptied the slots.

**The tick is a second slot in the `TimerQueue`, set at the pick, like the preemption deadline.** `TimerQueue` gains `m_tick_deadline`, folded into `earliest_deadline_locked()` (not held off, like the slice end), with `set_tick_deadline()` / the same refusal path. `Scheduler::set_next_thread()` arms it where it arms the preemption deadline and clears it where it clears that one:
```text
pick of a non-idle thread   preemption deadline = now + remaining slice
                            if TickSched::wanted() and the tick slot is empty: tick = next grid point
pick of the idle thread     preemption deadline = none; tick = none
tick interrupt              run_expired_timers(): the slot has passed:
                              slot = none; TickSched::on_tick(now) -> Scheduler::Current::tick(now)
                              if still wanted: slot = next grid point (forward by whole periods, no drift)
```
So "busy" and "idle" are the scheduler's own states, taken at the one place both edges are certain: a CPU woken by an IPI or an MWAIT write picks a thread and arms the tick; a CPU that returns from offline runs the idle loop with no tick and arms it at its first pick; nothing is armed on a CPU that halts. There is no idle-loop hook, no `CpuHotplugLevel` hook, no `Timer` to cancel, and no arming protocol: when the first consumer turns on (`TickSched::wanted()`), a busy CPU starts ticking at its next pick, at most a slice later, and when the last turns off each CPU's tick stops at its next fire.

**`Scheduler::Current::tick(now)`** runs in the CPU's timer interrupt, interrupts off, no run queue lock held, on the interrupted thread's context. It takes no PI lock (the old `tick_thread()` took the PI lock and then the run queue lock, and the PI lock is held with interrupts off across the trampoline hand-off). Today it does one thing: the balance trigger, `if period != 0 and now >= next_balance: next_balance = now + period; steal_work_from(online)` on its own scheduler (a per-CPU `next_balance`, touched only by its own CPU). It is the place the other periodic work goes.

**`TickSched::wanted()`** is a set of consumers, each a relaxed flag or a knob read: today the balance period non-zero. With none wanted nothing is armed and nothing changes from today: a busy CPU's interrupt rate stays at its slice rate (about 15 a second) and an idle one's at its heartbeat's. With the balance on, a busy CPU takes `HZ` (100) more a second, each a few microseconds, and the balance pass inside runs only when its period has passed.

**Both device modes.** In one-shot mode the slot above. In Periodic mode the device is the metronome: `TickSched::on_tick()` already runs on every tick of every CPU; it gains one line, calling `Scheduler::Current::tick(now)` when the CPU is running a thread that is not the idle thread and a consumer wants it. A Periodic CPU still wakes `HZ` times a second when idle: stopping that is the legacy mode's cost, as before, and a native periodic PIT or HPET cannot be stopped at all. The tick function is the same, and the periodic lane (`time_oneshot=false`) exercises it.

**Counters and reports.** `time.tick_sets` and `time.tick_runs` per CPU (as `TIMER_QUEUE_RUNS`), a column in `idle.stats`, and `idle.test` checks that an idle CPU takes no tick and, with a consumer on, that a busy CPU takes about `HZ` a second.

**Locking.** The tick slot is written under the queue's lock like the preemption deadline; the pick holds the run queue lock first (the existing order); the handler runs with the queue's lock dropped and calls `steal_work_from()`, which takes run queue locks only, one at a time. The balance's concurrency, one caller per thief and no phantom weight, is the argument of `.agents/tasks/periodic-balance/DESIGN.md` form B/C.

**Counterparts.** Linux: `struct tick_sched`, `tick_sched_timer()` / `tick_nohz_handler()` call `update_process_times()` and `sched_tick()`, which calls `sched_balance_trigger()`; `tick_nohz_idle_enter()` / `tick_nohz_idle_exit()` stop and restart it around the idle loop. Linux stops the tick in the idle loop with interrupts off and restarts it before `schedule_idle()`; here the pick is both edges, which is simpler and needs no timer cancel, because the tick is a deadline of the queue and the scheduler owns when the CPU is idle. Linux's wheel-side machinery (`get_next_timer_interrupt()`, `timer_clear_idle()`) and `tick_do_timekeeping` (`tick_do_timer_cpu`) have no counterpart: there is no wheel, and `SteadyClock` reads the HPET. Zircon is tickless and has no counterpart; this part follows Linux, which the user chose on 2026-11-06.

## Alternatives considered
- **The tick as a `Timer` in the queue** (the first version's shape): subject to the hold-off after a slow pass (up to 100 ms, so late exactly when the CPU is busiest), cancelled at offline before `take_over_from()` and re-armed by a hook, forbidden under the run queue lock (so not armable from the pick), and every cancel or arm that changes the queue's head writes the device. It is also what 1ede96ab removed.
- **Stop and restart in the idle loop** (Linux's shape): a restart there misses a CPU woken by MWAIT or one that returns from offline (`come_back()` never picks), and the stop would have to run before `until_next_deadline()` is read; per halt it would stop and start the tick at every heartbeat wake.
- **A lazy stop** (let the next tick fire and not re-arm): one interrupt for nothing per idle stretch, against a budget of 5 a second of which a quiet window already holds up to 3.
- **Always on while busy** (Linux's model, a tick at `HZ` on every busy CPU whether anything wants it): 100 interrupts a second for nothing when the balance is off, against about 15 today; the research's estimate is 6 to 9 microseconds each under the emulator. On demand costs a flag.
- **The tick drives the slice again:** rejected; `preempt.test` and the 65 ms turns rest on the deadline, and the deadline is never held off.
- **Calling `on_thread_tick` from the tick:** not now. It would run Fair's `update_thread` for the first time in a tick and is the user's code.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Decision (ii) of 2026-10-13, "no tick drives the scheduler" | amend it: a slice still ends at the preemption deadline, and a tick exists for periodic scheduler work only; reverse it: the Linux model, a tick on every busy CPU that also drives the slice | amend; the deadline's guarantees (once per switch, never held off) stay, and the tick costs nothing when nothing wants it | None (predictions are paused) | The user wants `TickSched` to work as intended (2026-11-06); this exact form is for confirmation |
| When the tick runs | on demand, while a consumer wants it; always on busy CPUs | on demand | None | |
| Shape of the tick in one-shot | a second slot in `TimerQueue`; a `Timer` | the slot | None | |
| Where the idle/busy edge is | the pick; the idle loop | the pick | None | |
| Periodic mode | the metronome calls the same function, no idle stopping; stop the tick while idle there too | no idle stopping | None | |
| What the tick function does | the balance trigger only; also the discipline's `on_thread_tick` | the balance only | None | |
| The rate | `HZ` (100), the balance period in whole ticks; a separate rate | `HZ` | None | |
| Prerequisite | fix the open flaky tests first (`preempt.test` after `tickreplace.test`, repair item 34; the hold-off check of `timer.test`); record baselines and exclude them | baselines, with the runs repeated | None | |

## Testing and verification
- Baselines before any code: per lane, `idle.stats` over a quiet window, `idle.test`, and `preempt.test`, `timer.test`, `timershare.test`, `tickreplace.test` five times each, `preemptdeadline.bench`; stored in `docs/perf/<YYMMDD>/tick-sched/`.
- Per step: `boot_check.py` on the default, Fair and four-node lanes and the `time_oneshot=false` lane; `ktest all`; the console tests `idle`, `preempt`, `timer`, `timershare`, `sleep`, `hotplug`, `tickreplace`, `tickbcast`, `tickfallback`, `dpc`, `steal`.
- The invariants as tests: an idle CPU takes no tick (`idle.test` budget unchanged); with a test consumer on, a busy CPU takes about `HZ` ticks a second and the tick counter agrees with the queue's; the slice still ends at the deadline (`preempt.test`: 65 ms turns); a CPU woken by a thread, by an IPI and by `come_back()` arms its tick at its first pick; a CPU taken offline and back has no tick while idle and one after its first pick; `tickreplace.test` with a consumer on; the Periodic lane runs the same function.
- Changes to existing tests the slot causes, to be made with the step: with a tick slot nearer than the slice end, the pick's preemption deadline is no longer the queue's head, so the pick stops writing the device (the baseline of repair item 1 and of `preemptdeadline.bench` change, for the better) and `tickreplace.test` case 6 cannot get its refusal from a pick the way it does now.
- Cannot be verified here: the interrupt cost on hardware (the emulator's figures are not hardware figures), and the Periodic idle cost on a machine whose idle is deeper than C1.

## Open questions
- Whether the tick should keep the whole device from a slow pass (the hold-off applies to timers, and the tick slot is not held off by design: a balance pass is bounded by `MAX_SCAN`, but this should be measured).
- Jiffies: they stand still in one-shot and are read only by the boot-time calibration; leaving them is decided, and a tick that advances them on the jiffies CPU is not wanted.
- Whether `TickSched` keeps its place as a `CpuLocal::Current` object holding the consumers and the counters, or the consumers belong to the scheduler.
- What `balance_tick` looks like when the discipline's `on_thread_tick` is later called: the order of the two in `tick(now)`.
