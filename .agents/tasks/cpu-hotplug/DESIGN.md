# cpu-hotplug: take a CPU offline at run time and bring it back

**Date**: 2026-10-12
**Status**: Accepted

## Problem
No code path takes a CPU offline and brings it back. The user built `CpuStopper` and the idle thread's `Offline`/`Suspend` states for it, and wants it for power saving ("Power Saving还是要做的", 2026-10-12). Today every place that clears a CPU's Online bit is a permanent halt (the suspend IPI, panic, the lockup detector's park), the AP bring-up code is `INIT_CODE` and freed after boot, the scheduler's migration functions are stubs, and neither the timer queue nor the Dpc queues can hand their work to another CPU. `docs/repairs/261007/sampler.md` item 5 and `docs/repairs/260909/x86-mp.md` item 2 wait on it.

Offlining is not what saves power: an offline CPU sits in the same C-state a deeply idle one reaches, and saves only the wakeups (Linux `Documentation/core-api/cpu_hotplug.rst` lists RAS, provisioning and suspend as the reasons). The power lever is deep idle (`mwait` C-states) and tickless idle, a separate piece of work. Hotplug is built for itself and for what it teaches: every per-CPU subsystem has to learn to hand its work away.

## Goals and non-goals
Goals:
- A kernel call takes one secondary CPU offline: nothing runs on it, no timer, Dpc or interrupt is left on it, and the rest of the system goes on.
- A kernel call brings it back, and it is as good as at boot.
- A kshell command and a test for each, round trip included.

Non-goals:
- The boot CPU: it takes every interrupt and owns the jiffies, as in Zircon, which refuses to unplug it.
- Power: `mwait` C-states and tickless idle are separate work.
- A userspace interface (Zircon's `zx_system_powerctl`), and the Fair discipline's side of migration (`fair.*` is the user's; it is out of the build).

## Design
Zircon's sequence, `mp_unplug_cpu_mask()` and `mp_hotplug_cpu_mask()` in `zircon/kernel/kernel/mp.cc`: one ordered function, no registration table.

- The control thread, on any CPU, shuts down the target's Dpc workers first (Zircon `DpcRunner::Shutdown()`), then asks the target's idle thread to go Offline and waits (Zircon `IdlePowerThread::TransitionActiveToOffline()`).
- The target's scheduler picks the idle thread while it has a pending transition, even with other threads runnable (Zircon `scheduler.cc:1115`). The idle thread, interrupts off, moves every queued thread that may run elsewhere to an active CPU and clears Active (Zircon `Scheduler::MigrateUnpinnedThreads()`), clears Online, stops its local timer, and halts (Zircon `mp_unplug_current_cpu()`).
- The control thread then takes the halted CPU's timers and queued Dpcs onto its own CPU (Zircon `TimerQueue::TransitionOffCpu()`, `DpcRunner::TransitionOffCpu()`).
- Bring-back re-runs the AP start (INIT-SIPI) with code kept resident, and the AP restarts its timer, Dpc workers and idle thread.
- Threads pinned only to the CPU going down: none may be left to wake there (Zircon's rule; a wake there asserts). The per-CPU kernel threads that are pinned, the Dpc workers, are shut down before and started again after.

## Alternatives considered
- Linux's cpuhp state machine (`kernel/cpu.c`): subsystems register callbacks, `stop_machine` clears Online, `balance_push` pushes tasks off through the stopper. Pays off with dozens of subsystems; here there are about seven, and it needs an all-CPU `stop_machine` and a stop scheduling class above every other, neither of which exists. Rejected on 2026-10-12.
- Zircon's order with the dying CPU's work run by `CpuStopper` and only the halt by the idle thread (Linux `take_cpu_down` and `play_dead`): uses both of the user's pieces, at the cost of a second hand-off. Not chosen.
- Linux's rule for pinned threads, that a per-CPU kernel thread may run on a CPU that is online but no longer active (`is_cpu_allowed()`): needed only if a CPU stays "inactive but running" for a while, which Zircon's sequence never does. Rejected on 2026-10-12.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Skeleton | a Zircon's ordered function; b Linux cpuhp; c Zircon's order, CpuStopper for the dying CPU's work | a | Not given | a, 2026-10-12 |
| Threads pinned to the CPU going down | i Linux: allowed while online; ii Zircon: none may wake there, Dpc workers stopped first | i, then ii once the whole Zircon sequence was taken | Not given | ii, 2026-10-12 |

## Testing and verification
- A `*.test` kernel command per step, run with `scripts/kshell_run.py` on dbg, and `ours-verify` before each commit.
- The round trip: offline a CPU, check nothing runs there and work still completes elsewhere, bring it back, check threads and timers land on it again.

## Open questions
- What becomes of a test or kernel thread pinned to a CPU that goes offline, beyond the Dpc workers: refuse the offline, or assert as Zircon does.
- Whether the offline CPU halts with `hlt` or parks in `mwait` on its state word, once deep idle exists.
