# cpu-hotplug: the steps to a CPU that goes offline and comes back

**Date**: 2026-10-12
**Status**: Accepted

## Request
"那就加呗，还能说啥，Power Saving还是要做的，我的CpuStopper和IdleThread就是为了热拔插做的" (2026-10-12), after `docs/repairs/261007/sampler.md` item 5 was asked about. Skeleton a and pinned-thread rule ii chosen the same day (DESIGN.md).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Concurrency homework | Write the arguments now; later | Now | Deferred by the user ("先把作业留着，我后面一起做", 2026-10-12): (1) a wakeup on another CPU racing the target clearing Active; (2) a Dpc enqueued while its worker is shutting down | Later, 2026-10-12 |

## Steps
Each step is introduced to the user, with its background, before it starts.

1. A CPU's Dpc workers can be stopped and started again: `DpcRunner::stop_workers()`, a worker that leaves its loop at a flag set under the queue lock and signals that it has (not joined: its own reap Dpc goes to the queue it stopped serving), Dpcs queued meanwhile left for the next worker; `dpc.test`. One commit.
2. A CPU goes offline and comes back, waiting with interrupts off rather than halting: the idle thread's transition and the scheduler's hook for it, the drain of the run queue, the closed IPI mailbox, the tick stopped and restarted, timers and Dpcs taken over, `task::take_cpu_offline()` and `bring_cpu_online()`, `cpu` command, `hotplug.test`. One commit.
3. Later steps are introduced one at a time.

Each step: clang-format and clang-tidy on its lines, `ours-verify`, its kshell test, a mutation where a test guards a line, a change record, and a commit of only its paths.

## Files
- Step 1: `kernel/include/ours/dpc.hpp`, `kernel/kernel/task/dpc.cpp`, `kernel/commands/dpc_test.cpp`, `kernel/commands/BUILD.gn`.
- Step 2: `kernel/kernel/task/cpu_hotplug.cpp`, `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/sched/scheduler.cpp`, `kernel/kernel/task/ipi_service.cpp`, `kernel/kernel/task/dpc.cpp`, `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/time/tick_device.cpp`, `kernel/kernel/time/tick_sched.cpp`, the headers they declare in, `kernel/include/ours/time/clock_source.hpp`, the two `tlb.hpp`, `kernel/commands/cpu.cpp`, `kernel/commands/hotplug_test.cpp`, `kernel/commands/dpc_test.cpp`.

## Test plan
`dpc.test: ok` on dbg for step 1; `hotplug.test: ok` on dbg, with `time_oneshot` on and off, from step 2.
