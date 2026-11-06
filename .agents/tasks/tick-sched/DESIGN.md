# tick-sched: TickSched as the scheduler's tick

**Date**: 2026-11-06
**Status**: Proposed

## Problem
`TickSched` was created to be the scheduler's tick (the user, 2026-11-06) and was implemented roughly. Today it is a flag that says the scheduler exists and an `on_tick()` that runs the timer queue, called only when a CPU's tick device is Periodic. The default images boot every CPU one-shot, where a CPU has no tick at all; the discipline hook `on_thread_tick` has no caller, and `Scheduler::Current::tick()` was deleted (c6764d82). So the scheduler has nowhere to hang periodic work. The first piece of such work is the balance of busy CPUs against each other (repair item 22; `.agents/tasks/periodic-balance/`), which three forms of a bespoke trigger (a worker thread, a timer per CPU, the slice end) were each a compromise for. The user wants `TickSched` to work as intended instead, and chose the model on 2026-11-06: Linux's. The builder picks between two tick modes (periodic, and nohz, a tick that stops while a CPU is idle), the tick checks the time slice, and the preemption deadline is the optional precise end of a slice (`sched_hptick`, Linux's `HRTICK`). That replaces decision (ii) of 2026-10-13 ("no tick drives the scheduler"), which the user chose against the recommended NO_HZ idle; the idle-power work that followed it is kept as the nohz mode with `sched_hptick` on.

How it came to be what it is (read by the research of 2026-11-06, commits cited): the first version (1387a1a3) was a self-re-arming tick `Timer` on every CPU with a `WorkMode`, `m_last_tick`, `m_next_tick`; 7eded2a4 dropped `WorkMode` as a second copy of the device's mode; the lockup heartbeat and the profiler moved to timers of their own (c61f59a5, 8e7557f1); c6764d82 moved the end of a slice to the preemption deadline, a slot in the CPU's `TimerQueue`; 1ede96ab removed the tick `Timer`, which fired every 10 ms on every CPU, idle ones included, taking an idle CPU from about 101 timer interrupts a second to about one; c65a75cc removed the two unused fields. That was the idle-power work, and decision (ii) of 2026-10-13 ("No tick drives the scheduler: Zircon's tickless model", chosen against the recommended Linux NOHZ idle) is written into `references/task.md`.

## Goals and non-goals
Goals:
- `TickSched` owns the tick: a periodic tick of the scheduler at `HZ`, and one scheduler function (`Scheduler::Current::tick(now)`) that the tick calls, from which the scheduler's periodic work hangs: charging the running thread and asking for a reschedule when its slice is used up (through the discipline's `on_thread_tick`), and the balance trigger.
- Two modes, chosen by the builder with one build argument `sched_tick_mode`, the way Linux's Kconfig offers `HZ_PERIODIC` and `NO_HZ_IDLE`: **periodic**, the tick runs on every CPU all the time; **nohz**, the tick runs while a CPU is busy and is stopped while it is idle. The argument replaces `sched_nohz`, which has no reader (repair item 20).
- The end of a slice is checked at the tick. `sched_hptick` (it exists, default false) keeps the preemption deadline as the precise end: a slot in the CPU's `TimerQueue`, as now. With it on, the images keep the timing the idle-power work measured; with it off, a slice ends at the first tick past it.
- Both device modes (one-shot and periodic) run the same tick function; nohz needs a one-shot device and falls back to the periodic behaviour on a CPU that has none, as Linux's does.
- The tick is counted and reported, and the tests that pin interrupt counts cover each mode.
- The periodic balance (repair item 22, `.agents/tasks/periodic-balance/`) is the first consumer beyond the slice: the knob `balance-period-ms`, off by default.

Non-goals: jiffies (they stay, standing still in one-shot: the user's call of 2026-10-23); timekeeping, RCU and full-NO_HZ duties (`NO_HZ_FULL`, a tick stopped on a busy CPU), which Linux's `tick_sched` carries and this kernel does not need; Fair's code (`fair.*` is the user's: where the tick exercises it for the first time and finds a defect, it goes to `docs/repairs`); a nohz-style kick of halted CPUs (repair item 23, a later consumer); deep-idle broadcast; renaming or folding `TickSched`.

## Design
**Facts the design rests on** (research of 2026-11-06, `file:line` from the three reports; the slot mechanism below was re-read by hand):
- `TimerQueue::reprogram_locked()` is the only writer of a CPU's one-shot device; it arms for `earliest_deadline_locked()` = the earliest of the timers (held off after a slow pass) and `m_preemption_deadline` (never held off) (`timer_queue.cpp:72-123`).
- `Scheduler::set_next_thread()` arms the preemption deadline at every pick of a non-idle thread, the running thread picked again included, and clears it for the idle thread, under the run queue lock; that lock comes before the queue's. A refusal of the device there is not passed on; it is a self timer-IPI, and the queue passes it on with no lock held (`scheduler_inline.hpp:41-65`, `timer_queue.cpp:171-195`).
- `Timer::activate()` and `cancel()` call `notify_refusal()`, which may not run under the run queue lock, so a `Timer` cannot be armed from the pick (`timer_queue.cpp:182-191, 332`).
- The idle/busy edge that exists everywhere is the pick. The restart cannot be an interrupt: with MWAIT a waker writes the monitor word and no interrupt happens, and `come_back()` enters the idle loop without picking (`idle_thread.cpp`, `arch/x86/kernel/task/idle_thread.cpp:155-162`).
- A one-shot CPU has no tick; a busy one is interrupted by its timers, its 1 Hz lockup heartbeat and its slice end (about 15 a second); an idle one about once a second. A Periodic CPU's device fires every `1/HZ` busy or idle, and `program_event()` is a no-op there (`tick_device.cpp:56-76`). `idle.test` allows 5 timer interrupts per CPU per second (`MAX_QUIET_INTERRUPTS`).
- `take_over_from()` moves every queued `Timer` of a CPU going offline to the survivor; a deadline slot is not a `Timer` and is not moved. A CPU goes offline from its idle thread, whose pick has emptied the slots.

**The tick in one-shot mode is a second slot in the `TimerQueue`, set at the pick, like the preemption deadline.** `TimerQueue` gains `m_tick_deadline`, folded into `earliest_deadline_locked()` (not held off, like the slice end), with `set_tick_deadline()` / the same refusal path. `Scheduler::set_next_thread()` arms it where it arms the preemption deadline and clears it where it clears that one:
```text
pick of a non-idle thread   preemption deadline = now + remaining slice, only if sched_hptick
                            if the tick slot is empty: tick = next grid point
pick of the idle thread     preemption deadline = none
                            nohz: tick = none        periodic: the tick slot is left armed
tick interrupt              run_expired_timers(): the slot has passed:
                              slot = none; TickSched::on_tick(now) -> Scheduler::Current::tick(now)
                              slot = next grid point (forward by whole periods, no drift)
```
The handler re-arms the slot unconditionally, in both modes; what differs is the idle pick, which in nohz mode clears it. The tick that was pending when the CPU went idle cannot fire, because the clear is in the same pick, and a tick that fires on a CPU that is busy is re-armed for the next period.

So "busy" and "idle" are the scheduler's own states, taken at the one place both edges are certain: a CPU woken by an IPI or an MWAIT write picks a thread and arms the tick; a CPU that returns from offline runs the idle loop with no tick and arms it at its first pick; nothing is armed on a CPU that halts. There is no idle-loop hook, no `CpuHotplugLevel` hook, no `Timer` to cancel, and no arming protocol: the tick needs no consumer to start it, because it runs whenever the CPU is busy.

**`Scheduler::Current::tick(now)`** runs in the CPU's timer interrupt, interrupts off, no run queue lock held on entry, on the interrupted thread's context. It takes the run queue lock and not the PI lock (the old `tick_thread()` took the PI lock first, and the PI lock is held with interrupts off across the trampoline hand-off). It does, in this order:
1. Charge the running thread and ask the discipline about its slice: `on_thread_tick(queued = false)`, which asks for a reschedule (`add_pending()`, paid at the interrupt's exit) when the slice is used up. Round-robin's hook is empty today and gets the accounting (the clock update and the remaining slice), since a slice no longer ends only at the deadline; Fair's hook exists and has never run. With `sched_hptick` on, the deadline ends the slice first and the tick finds nothing to do.
2. The balance trigger: `if period != 0 and now >= next_balance: next_balance = now + period; steal_work_from(online)` on its own scheduler (a per-CPU `next_balance`, touched only by its own CPU).

It is the place the other periodic work goes.

**The modes and what each costs.** The tick runs whenever the mode says so, with or without a consumer, as Linux's does (the user's choice, 2026-11-06): in nohz a busy CPU takes `HZ` (100) interrupts a second, each a few microseconds, against about 15 with only the deadline and the heartbeat; an idle CPU takes none from the tick and about one a second in all. In periodic every CPU takes 100 a second, idle ones included, which is what the idle-power work removed and is what the builder who picks periodic accepts. `idle.test`'s quiet check applies to nohz; in periodic it is skipped, as it is today on the periodic device.

**Both device modes.** In one-shot mode the slot above. In Periodic mode the device is the metronome: `TickSched::on_tick()` already runs on every tick of every CPU; it gains one line, calling `Scheduler::Current::tick(now)` when the CPU is running a thread that is not the idle thread (nohz) or always (periodic). A Periodic CPU still wakes `HZ` times a second when idle, nohz or not: a native periodic PIT or HPET cannot be stopped, and stopping the emulated periodic local APIC would need `program_event()` to stop being a no-op there. So nohz is a property of a one-shot CPU, and a Periodic CPU in nohz mode behaves as periodic with the idle thread's ticks not calling the scheduler. The tick function is the same in all of them, and the periodic lane (`time_oneshot=false`) exercises it.

**The combinations** the build can make, and what they are:
| `sched_tick_mode` | `sched_hptick` | What a slice and the tick do |
|---|---|---|
| nohz | on | the idle-power timing (the slice ends at the deadline, 65 ms turns), plus a tick on busy CPUs for the balance and the other periodic work |
| nohz | off | Linux's `NO_HZ_IDLE`: the slice ends at the first tick past it (10 ms granularity) |
| periodic | on | a constant tick everywhere and a precise slice end |
| periodic | off | Linux's `HZ_PERIODIC` |
The lanes of the verification matrix cover each row.

**Counters and reports.** `time.tick_sets` and `time.tick_runs` per CPU (as `TIMER_QUEUE_RUNS`), a column in `idle.stats`, and `idle.test` checks that an idle CPU takes no tick and, with a consumer on, that a busy CPU takes about `HZ` a second.

**Locking.** The tick slot is written under the queue's lock like the preemption deadline; the pick holds the run queue lock first (the existing order); the handler runs with the queue's lock dropped and calls `steal_work_from()`, which takes run queue locks only, one at a time. The balance's concurrency, one caller per thief and no phantom weight, is the argument of `.agents/tasks/periodic-balance/DESIGN.md` form B/C.

**Counterparts.** Linux: the `choice` "Timer tick handling" in `kernel/time/Kconfig` (`HZ_PERIODIC`, `NO_HZ_IDLE`, `NO_HZ_FULL`); `sched_feat(HRTICK)` for the precise slice end; `struct tick_sched`, `tick_sched_timer()` / `tick_nohz_handler()` call `update_process_times()` and `sched_tick()`, which calls the class's `task_tick()` (the slice) and `sched_balance_trigger()`; `tick_nohz_idle_enter()` / `tick_nohz_idle_exit()` stop and restart it around the idle loop. Linux stops the tick in the idle loop with interrupts off and restarts it before `schedule_idle()`; here the pick is both edges, which is simpler and needs no timer cancel, because the tick is a deadline of the queue and the scheduler owns when the CPU is idle. Linux's wheel-side machinery (`get_next_timer_interrupt()`, `timer_clear_idle()`) and `tick_do_timekeeping` (`tick_do_timer_cpu`) have no counterpart: there is no wheel, and `SteadyClock` reads the HPET. Zircon is tickless (its slice ends at `TimerQueue::PreemptReset()`'s deadline), which is what `sched_hptick` keeps; the tick follows Linux, which the user chose on 2026-11-06.

## Alternatives considered
- **The tick as a `Timer` in the queue** (the first version's shape): subject to the hold-off after a slow pass (up to 100 ms, so late exactly when the CPU is busiest), cancelled at offline before `take_over_from()` and re-armed by a hook, forbidden under the run queue lock (so not armable from the pick), and every cancel or arm that changes the queue's head writes the device. It is also what 1ede96ab removed.
- **Stop and restart in the idle loop** (Linux's shape): a restart there misses a CPU woken by MWAIT or one that returns from offline (`come_back()` never picks), and the stop would have to run before `until_next_deadline()` is read; per halt it would stop and start the tick at every heartbeat wake.
- **A lazy stop** (let the next tick fire and not re-arm): one interrupt for nothing per idle stretch, against a budget of 5 a second of which a quiet window already holds up to 3.
- **Keeping the deadline as the only end of a slice and the tick for periodic work only** (the first plan): fewer changes, but not Linux's relation between the tick and the slice, which the user chose against on 2026-11-06.
- **A tick that runs only when a consumer wants it** (nohz): saves the busy CPU's 100 interrupts a second while the balance is off; the user chose Linux's tick that always runs on a busy CPU.
- **`NO_HZ_FULL`** (no tick on a busy CPU with one thread): out of scope.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Model | keep decision (ii), the tick only for periodic work; Linux's: a builder's choice of periodic and nohz, the tick checks the slice, the deadline is the optional precise end | the user's | None (predictions are paused) | Linux's model, two modes chosen by the builder (the user, 2026-11-06) |
| The end of a slice | the deadline in both modes; the tick checks it, the deadline optional (`sched_hptick`) | the user's | None | the tick checks it, `sched_hptick` is the precise end (the user, 2026-11-06) |
| The busy tick in nohz | always on a busy CPU; only while a consumer wants it | the user's | None | always (the user, 2026-11-06) |
| Defaults of the dev images | `nohz` with `sched_hptick` on (the idle-power timing, `preempt.test`'s 65 ms turns); `nohz` with it off (Linux's default) | nohz with hptick on, and the other three rows on lanes | None | nohz with `sched_hptick` on; the other three rows on lanes (the user, 2026-11-06) |
| The build argument | `sched_tick_mode = "periodic" or "nohz"` replacing `sched_nohz`; also a boot override like Linux's `nohz=off` | the build argument; the override later if wanted | None | |
| Shape of the tick in one-shot | a second slot in `TimerQueue`; a `Timer` | the slot | None | |
| Where the idle/busy edge is (nohz) | the pick; the idle loop | the pick | None | |
| Periodic device in nohz mode | behaves as periodic, the idle thread's ticks do not call the scheduler; stop the emulated periodic APIC's tick while idle | the first | None | |
| What the tick function does | charge and ask the discipline (`on_thread_tick`), then the balance trigger | that | None | |
| Fair's hook | the tick calls Fair's `on_thread_tick` as it stands and defects go to `docs/repairs`; Fair is excluded from the tick until the user adapts it | the first, since the tick-checked slice needs it | None | |
| The rate | `HZ` (100), the balance period in whole ticks; a separate rate | `HZ` | None | |
| Prerequisite | fix the open flaky tests first (`preempt.test` after `tickreplace.test`, repair item 34; the hold-off check of `timer.test`); record baselines and exclude them | baselines, with the runs repeated | None | |

## Testing and verification
- Baselines before any code: per lane, `idle.stats` over a quiet window, `idle.test`, and `preempt.test`, `timer.test`, `timershare.test`, `tickreplace.test` five times each, `preemptdeadline.bench`; stored in `docs/perf/<YYMMDD>/tick-sched/`.
- Per step: `boot_check.py` on the default, Fair and four-node lanes and the `time_oneshot=false` lane, and, from the step that adds the mode argument, on the other rows of the combination table (nohz with hptick off, periodic with it on and off); `ktest all`; the console tests `idle`, `preempt`, `timer`, `timershare`, `sleep`, `hotplug`, `tickreplace`, `tickbcast`, `tickfallback`, `dpc`, `steal`.
- The invariants as tests: in nohz an idle CPU takes no tick (`idle.test` budget unchanged) and a busy one takes about `HZ` ticks a second, the tick counter agreeing with the queue's; in periodic every CPU takes about `HZ`; with `sched_hptick` on a slice still ends at the deadline (`preempt.test`: 65 ms turns), and with it off at the first tick past it (the same test with the tick's granularity as its tolerance: it must be written for that row); a CPU woken by a thread, by an IPI and by `come_back()` arms its tick at its first pick; a CPU taken offline and back has no tick while idle and one after its first pick; `tickreplace.test` with a consumer on; the Periodic lane runs the same function.
- Changes to existing tests the slot causes, to be made with the step: with a tick slot nearer than the slice end, the pick's preemption deadline is no longer the queue's head, so the pick stops writing the device (the baseline of repair item 1 and of `preemptdeadline.bench` change, for the better) and `tickreplace.test` case 6 cannot get its refusal from a pick the way it does now.
- Cannot be verified here: the interrupt cost on hardware (the emulator's figures are not hardware figures), and the Periodic idle cost on a machine whose idle is deeper than C1.

## Open questions
- How a tick-checked slice is charged: the run queue's clock is updated at the pick and at the reschedule; the tick updates it too (as Linux's `update_curr()`), and whether that changes `SchedClock`'s "skip the next update" optimisation after an enqueue.
- Whether the tick should keep the whole device from a slow pass (the hold-off applies to timers, and the tick slot is not held off by design: a balance pass is bounded by `MAX_SCAN`, but this should be measured).
- Jiffies: they stand still in one-shot and are read only by the boot-time calibration; leaving them is decided, and a tick that advances them on the jiffies CPU is not wanted.
- Whether `TickSched` keeps its place as a `CpuLocal::Current` object holding the consumers and the counters, or the consumers belong to the scheduler.
