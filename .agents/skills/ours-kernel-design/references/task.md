# Tasks, scheduling, locking, time

## Scheduler

- Fair (`kernel/kernel/task/sched/fair.*`) is the user's own design and out of
  the build. Bring-up runs on a simple round-robin `ISchedule`. Do not edit
  Fair at all; a refactor whose value depends on changing fair.cpp must be put
  to the user, not carried out. In the generic scheduler machinery
  (`scheduler.cpp`, `context.hpp`, `scheduler_inline.hpp`), ask before any fix
  beyond a plain seeding or ordering defect.
- The framework around the algorithm may be restructured boldly; what the user
  protects is the scheduling algorithm, not the size of a change.
- Numerics follow Zircon: weights and times are `ffl::Fixed`
  (`SchedWeight` is `ffl::Fixed<int64_t, 10>`, a fraction in (0, 1]). No Linux
  integer weights, no `wmult` reciprocal table. Read
  `zircon/kernel/include/kernel/scheduler_state.h` before designing any
  scheduler primitive.

## Thread and wait-queue locking

- Never propose a global thread lock. `Thread` (`m_pi_lock`,
  `m_resource_lock`), `WaitQueue` (`m_lock`) and each per-CPU run queue
  (`rqlock`) lock separately. What is missing is the protocol for taking an
  arbitrary priority-inheritance chain of those locks: Zircon's ChainLock
  (`zircon/system/ulib/concurrent/include/lib/concurrent/chainlock.h`, design in
  `zircon/kernel/lib/kconcurrent/docs/chainlock.md`). The
  `OwnedWaitQueue::lock_pi_chain_*` stubs are reserved for it, and the empty
  `system/lib/concurrent` exists to hold it.
- `Thread`'s two locks follow Linux's `task_struct`, not Zircon:
  `m_pi_lock` is `p->pi_lock` (PI edges, state transitions, wakeup
  serialisation; nests outside the run-queue lock), and `m_resource_lock` is
  `alloc_lock`/`task_lock()` (the resources a thread holds; never on a
  scheduler path).

## Small concurrent state

For a lifecycle state plus configuration fields set together (the prof
`Sampler` is the case): exactly one atomic, the state. The other fields stay
plain, are written only while no reader acts on them, and are published by the
release store to the running state. Never pack several fields into one atomic
word.

## Time

`kernel/time` has no TimerScheduler layer: `TimerQueue` is the per-CPU timer
engine and does that work itself.
