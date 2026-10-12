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
2. Later steps are introduced one at a time.

Each step: clang-format and clang-tidy on its lines, `ours-verify`, its kshell test, a mutation where a test guards a line, a change record, and a commit of only its paths.

## Files
- Step 1: `kernel/include/ours/dpc.hpp`, `kernel/kernel/task/dpc.cpp`, `kernel/commands/dpc_test.cpp`, `kernel/commands/BUILD.gn`.

## Test plan
`dpc.test: ok` on dbg for step 1; the round trip of DESIGN.md's Testing section once a CPU can go offline.
