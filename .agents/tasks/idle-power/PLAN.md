# idle-power: the steps from a measured baseline to tickless and deep idle

**Date**: 2026-10-13
**Status**: Accepted

## Request
"做省电吧" (2026-10-13), then "b和ii": deep idle and tickless both, tickless after Zircon (DESIGN.md).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Measurement before change | Counters and a command first; change first | Counters first | Not given | Counters first, 2026-10-13 |
| Concurrency homework | Write the argument now; later | Now | Deferred, as for cpu-hotplug: (1) why the halt clock's sequence count lets another CPU read a halt in progress without a torn or double count (`idle_thread.cpp`, `HaltClock`); (2) why the run queue lock may be held while the timer queue's is taken (`Scheduler::set_next_thread()`, `TimerQueue::reset_preempt()`), and what would deadlock if a timer's handler ran under the queue lock; (3) why a waker that finds a CPU's monitor word saying Waiting may skip the IPI: every order of the idle CPU's store, MONITOR, check and MWAIT against the waker's exchange (`ArchIdleThread::idle()`, `wake()`) | Later, 2026-10-13 |
| Step 6's scope | A table, leaf 5 filter and host-tested governor, C1 alone enabled, step 7 to repairs; B steps 6 and 7 whole; C stop the task here | A | Not given | A, 2026-10-14 |
| The governor's rule | Zircon's, the last idle stretch alone; that, bounded by the time until the next deadline | Not given | Not given | Bounded by the deadline, chosen for the user, who did not answer (2026-10-14); theirs to overrule |
| The milestone's hand-written function | `IdleStates::pick()` | `pick()` | Not given | Declined ("不手写", 2026-10-14) |
| The two "gibberish" threads `labour_routine()` starts on every image (`kernel/top/main.cpp`), which keep two CPUs from ever halting | Delete them; start them only under a boot option; keep them | A boot option, off by default | Not given | Delete them (ii), 2026-10-13 |

## Steps
Each step is introduced to the user, with its background, before it starts.

1. Measurement: per-CPU counters for local timer interrupts and idle entries, idle time accounted around the halt only, and a kshell command reporting them over a window; the baseline recorded. One commit.
2. The lockup heartbeat moves off the tick onto a per-CPU one-second timer, cancelled and re-armed as a CPU goes offline and comes back (lockup-watchdog D1 (b)). One commit.
3. A slice ends at its CPU's preemption deadline, a slot in the timer queue (Zircon `TimerQueue::PreemptReset()`), set by the scheduler at every pick and emptied for the idle thread; the tick no longer calls the scheduler; `preempt.test`. Fixes `docs/repairs/261009/kernel-time.md` item 2. One commit.
4. The tick `Timer` goes: a one-shot CPU is armed only for its timers and preemption deadline, and jiffies advance only from a periodic tick, the PIT's at boot for the local APIC's calibration (decision A in DESIGN.md); `idle.stats` reports tenths, and `idle.test` checks that, with no tick, no idle CPU is woken for nothing. One commit.
5. An idle CPU waits in MWAIT (C1) on a per-CPU monitor word when CPUID reports MONITOR, in HLT otherwise, and a reschedule to a CPU waiting on its word writes the word instead of sending an IPI; `idle.stats` says which and counts the waits a write ended, and `idle.test` checks that waking an idle CPU writes its word. One commit.
6. The idle governor (A, the minimal step 6): each model's table of MWAIT states, cut to those CPUID leaf 5 reports, C1 alone enabled; `IdleStates::pick()`, Zircon's rule bounded by the next deadline, host-tested; `idle()` enters what it picks. States that stop the local APIC timer (step 7) go to `docs/repairs/261014/x86-idle.md`. Two commits.
7. Later steps are introduced one at a time.

Each step: clang-format and clang-tidy on its lines, `ours-verify`, its kshell tests, a mutation where a test guards a line, a change record, and a commit of only its paths.

## Files
- Step 1: `kernel/lib/gktl/include/gktl/counter.hpp`, `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/task/idle_thread.cpp`, their headers, `kernel/commands/idle.cpp`, `kernel/commands/BUILD.gn`.
- Step 2: `kernel/lib/lockup_detector/src/checker.cpp`, `src/report.cpp`, `include/lockup_detector/lockup_detector.hpp`, `README.md`, `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn`, `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/BUILD.gn`, `scripts/kshell_run.py`.
- Step 3: `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/task/sched/scheduler.cpp`, `sched_clock.cpp`, `rr.cpp`, their headers, `kernel/include/ours/task/sched/scheduler_inline.hpp`, `sched_entity.hpp`, `run_queue.hpp`, `kernel/commands/preempt_test.cpp`, `kernel/commands/BUILD.gn`.
- Step 4: `kernel/kernel/time/tick_device.cpp`, `kernel/kernel/time/tick_sched.cpp`, their headers, `kernel/include/ours/time/clock_source.hpp`, `kernel/commands/idle.cpp`, comments in `kernel/commands/hotplug_test.cpp` and `timer_test.cpp`.
- Step 5: `kernel/arch/x86/kernel/task/idle_thread.cpp`, its header, `kernel/arch/x86/kernel/mp.cpp`, `kernel/arch/x86/kernel/feature.cpp`, its header, `kernel/arch/x86/lib/arch/include/arch/x86/processor/feature.hpp`, `kernel/commands/idle.cpp`.
- Step 6: `kernel/arch/x86/lib/idle_states/` (new), `kernel/arch/x86/kernel/task/idle_thread.cpp`, its header, `kernel/arch/x86/kernel/init.cpp`, `kernel/arch/x86/kernel/BUILD.gn`, `BUILD.gn`, `kernel/kernel/task/idle_thread.cpp`, its header, `kernel/kernel/time/timer_queue.cpp`, its header.

## Test plan
The measurement command on a quiet dbg image before and after every step, its numbers in each change record.
