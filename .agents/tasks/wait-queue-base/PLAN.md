# wait-queue-base: Share the wait queue mechanism between sibling queues

**Date**: 2026-11-12
**Status**: Accepted

## Request

Give `WaitQueue` and `OwnedWaitQueue` a common `WaitQueueBase`. The two concrete types inherit directly from it.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Queue hierarchy | Ordinary queue as base or common base | Common base | None | Common base, 2026-11-12 |

## Steps

1. Extract the shared waiter storage and operations and migrate the two queue classes and waiter back-pointer in one commit. Verify timeout and suspend on ordinary waits, timeout on an owned wait, and notification through mutex handoff.

## Files

- `kernel/include/ours/task/wait_queue_base.hpp` and `kernel/kernel/task/wait_queue_base.cpp` contain the shared implementation.
- The ordinary and owned queue files contain their concrete interfaces and operations.
- The task type declarations, thread friendship and GN source set reference the common base.
- The kernel wait queue test covers a targeted owned-queue timeout through the shared back-pointer.

## Test plan

Run changed-line clang-format, clang-tidy and GN formatting. Build and boot dbg and rel and run the host tests with RR and Fair. Run the WaitQueue and MutexStress kernel suites.
