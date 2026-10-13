# idle-power: idle CPUs that are not woken for nothing, and sleep deeper when they can

**Date**: 2026-10-13
**Status**: Accepted

## Problem
The user wants power saving (2026-10-12, "Power Saving还是要做的"; chosen as the next task 2026-10-13). An idle CPU here executes `sti; hlt` and is woken every 10 ms by its tick: in one-shot mode the tick is a `Timer` in the CPU's `TimerQueue` that re-arms itself for ever, idle or not, so an idle CPU takes about 100 timer interrupts a second for nothing. Nothing detects MONITOR/MWAIT, its C-state hints or ARAT, and nothing counts timer interrupts, idle entries or idle time, so neither the waste nor a fix can be measured today.

Facts that shape the design:
- Timekeeping does not need the tick: `SteadyClock` reads the HPET counter; jiffies feed only the LAPIC calibration at boot.
- Three things use the tick: the lockup detector's heartbeat and checks, round-robin slice expiry, and the jiffies owner (the boot CPU).
- The test machine is QEMU TCG with `-cpu Nehalem`: no MONITOR/MWAIT unless asked for (`+monitor`), and then TCG treats MWAIT as PAUSE with more than one CPU, so deep idle states can be exercised but not measured.

## Goals and non-goals
Goals:
- Measure first: per CPU, timer interrupts, idle entries and time spent halted, and a command that reports them over a window.
- No periodic tick (Zircon's model, decision ii): a CPU's timer device is armed for its next real deadline, which on a busy CPU includes the running thread's preemption deadline and on an idle one may be none.
- Deep idle (decision b): MWAIT with a C-state hint where CPUID offers it, chosen by a governor, falling back to HLT; a state that stops the LAPIC timer is entered only on a CPU with ARAT, or with its deadline handed to a broadcast device.

Non-goals:
- ACPI `_CST` and an AML interpreter; P-states and frequency scaling; system suspend.
- The Fair discipline's preemption (`fair.*` is the user's; it is out of the build and keeps its own path until the user moves it).

## Design
- Measurement: per-CPU counters for local timer interrupts and idle entries, idle time accounted around the halt only, and a kshell command that samples them over a window.
- Tickless (Zircon `TimerQueue::PreemptReset()`, `zircon/kernel/kernel/timer.cc`): each CPU's `TimerQueue` gains a preemption-deadline slot, set by the scheduler at every pick (`Scheduler::set_next_thread()`) from the thread's remaining slice and emptied for the idle thread, and the device is armed for the earliest of that slot and the queued timers; a passed deadline only asks for a reschedule (Zircon `Scheduler::TimerTick()`). The tick `Timer` goes; its consumers move: slice expiry to the deadline, the lockup heartbeat to a per-CPU 1 s `Timer` (D1 (b) of `.agents/tasks/lockup-watchdog/DESIGN.md`, decided for this case), jiffies to boot only.
- Deep idle (Zircon `X86IdleStates`, `zircon/kernel/arch/x86/idle_states.cc`): CPUID leaf 1 MONITOR, leaf 5 sub-states and leaf 6 ARAT detected; a state table and a governor pick a state; the idle thread waits in MWAIT on a per-CPU monitor word, which a waker writes instead of sending an IPI. Where Zircon leaves the LAPIC timer stopping in a deep state unhandled, a state that stops it uses the existing `TickBroadcast`.

## Alternatives considered
- (a) Measurement and tickless only, deep idle deferred until real hardware or KVM can show it: recommended, not chosen; the user chose to build both (b), 2026-10-13.
- (i) Linux NOHZ idle, the tick stopped only on idle CPUs: smaller, leaves the scheduler tick-driven; not chosen; the user chose Zircon's tickless model (ii), 2026-10-13, settling what `references/task.md` left as "a later decision".

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope | a measure and tickless, deep idle later; b both | a | Not given | b, 2026-10-13 |
| Tickless model | i Linux NOHZ idle; ii Zircon, no periodic tick | i | Not given | ii, 2026-10-13 |
| Lockup heartbeat without a tick | a skip idle CPUs; b per-CPU 1 s Timer | b | Not given | b, decided in lockup-watchdog D1 for this case |
| Jiffies once the tick Timer goes | A kept for the local APIC's calibration at boot, standing still after; B removed, the calibration timed by the HPET | A, B as a repair item | Not given | A, 2026-10-13 ("按你推荐的走吧") |

## Testing and verification
- The measurement command on a quiet dbg image, before and after each step: timer interrupts per idle CPU per second and the share of time halted.
- `ours-verify`, and the scheduler, timer, lockup and hotplug kshell tests, after each step.
- Deep idle with `CPU=Nehalem,+monitor,+arat` and without: the MWAIT path runs and the fallback works; power is not measurable under TCG.

## Open questions
- The deep-idle governor: Zircon's (deepest state whose exit latency is under three times the last idle stretch) or one that also uses the next deadline, as Linux's menu governor does.
- Which per-microarchitecture state tables to carry, given that QEMU's models are not real parts.
