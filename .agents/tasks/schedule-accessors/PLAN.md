# schedule-accessors: Expose named methods on erased schedule references

**Date**: 2026-11-07
**Status**: Accepted

## Request

Follow the Proxy member-call model for scheduler dispatch. Check the lock requirement against the run queue supplied to each named operation.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Member access | Tagged calls or generated named accessors | Named accessors on Ref with the lock contract declared by Schedule | None | Named accessors, 2026-11-07 |

## Steps

1. Add dispatch-generated member accessors to Ref and use them throughout Scheduler. Check named dispatch and lock analysis, then boot both development images and commit the change with its record.

## Files

- `system/lib/ustl/include/ustl/dyn.hpp` defines and combines the member accessors.
- `system/lib/ustl/test/dyn_test.cpp` exercises the named accessors on existing targets.
- `kernel/include/ours/task/sched/schedule.hpp` declares the lock contract.
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/include/ours/task/sched/scheduler_inline.hpp`, and `kernel/kernel/task/sched/scheduler.cpp` migrate scheduler calls.
- `docs/changes/261107/kernel-task-sched.md` records the change and verification.

## Test plan

Check the host dynamic-dispatch test and compile enqueue calls with the correct lock, with no lock and with another queue's lock. Run changed-line clang-format, clang-tidy and the full host suite. Boot both images and run targeted scheduler tests under QEMU.
