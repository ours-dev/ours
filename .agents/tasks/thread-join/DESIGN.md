# thread-join: make Thread::join() work again, as Zircon's Thread::Join() does

**Date**: 2026-10-03
**Status**: Proposed

## Problem
`Thread::join()` returns `Status::NotSupported` (`kernel/kernel/task/thread.cpp`), and `Thread::Current::exit()` panics on a thread that exits joinable. The user wrote a join before. `d5a7477e` (2026-09-24) replaced it, because it could not work and nothing called it. The record is in `docs/changes/260924/kernel-task.md`, the section starting at line 351. The user asked on 2026-10-03 for join to be brought back as a task of its own.

What was wrong with the old body, and is still missing, compared with Zircon's `Thread::Join()` (`zircon/kernel/kernel/thread.cc:774`):
- **Who sleeps.** The old body called `m_task_state.join(this, deadline)` under `DEBUG_ASSERT(this == Current::get())`. It parked the thread being joined, so it worked only for a thread joining itself. Zircon asserts `this != current_thread` and parks the caller on the joined thread's `task_state_` queue.
- **The check-then-park window.** "Is it Dead yet?" and the park must be one step against `exit()`'s `set_state_dead()` and wake. Otherwise the wake can land in between, and the joiner sleeps for good. `WaitQueue::park_thread_preamble_locked()` has the right shape.
- **Who frees the thread.** `exit()` queues a Dpc that calls `destroy()` for every thread that is not NoFree. For a joinable thread the joiner frees it, so `exit()` must not queue that Dpc. It must also go into its last reschedule still holding its PI lock, so the joiner cannot free the stack `exit()` is running on. In Zircon this is `queue_free_dpc` in `Thread::Current::Exit()` (`thread.cc:986`, `:1101`).
- **The global list.** The joiner must unlink the thread from `SPAWNED_THREADS`, as Zircon's `Join()` calls `EraseFromListsLocked()` (`thread.cc:767`, `:836`). `destroy()` does not.
- **`detach()` of a Dead thread** calls `join()` with the PI lock still held. Zircon's `Detach()` (`thread.cc:858`) releases it first.
- **Lock nesting.** Waking joiners takes their PI locks inside the exiting thread's. This kernel has no global thread lock, by design. Zircon handles the same nesting with chain-lock backoff (`ChainLockTransaction`, `AcquireBothOrBackoff`, and `TryWakeJoiners()` failing on backoff at `thread.cc:260`, `:1092`). This kernel's thread locks have no backoff yet.

## Goals and non-goals
Goals:
- A kernel thread can be spawned joinable, and another kernel thread can `join()` it, get its return code, and have it freed, with a deadline.
- `exit()` of a joinable thread wakes its joiners instead of panicking.
- `detach()` of a thread that is already Dead reaps it without deadlocking.

Non-goals:
- Joining user threads. They are always detached; `ThreadDispatcher` and process teardown own them.
- A general chain-lock facility, unless the lock-order decision below needs one.

## Design
To be designed in this task. The objects involved:
- `Thread::m_task_state`: the return code and `m_result_wait_queue`, the queue joiners park on.
- The thread's PI lock, `get_pi_lock()`, which guards its state and `m_flags` (Detached, NoFree).
- `SPAWNED_THREADS` and `SPAWNED_THREADS_LOCK`.
- The free Dpc that `exit()` queues today.

Zircon's order of events, to compare against:
1. The joiner locks the global list and the target. If the target is Detached, it fails with BAD_STATE.
2. If the target is not Dead, the joiner trades those locks for its own lock and the wait queue's (with backoff), parks, and retries from step 1 once woken.
3. If the target is Dead, the joiner reads the return code, unlinks the thread from the lists, drops its locks, and frees the thread.
4. On the exit side, the dying thread sets itself Dead under its lock, wakes the joiners (retrying on backoff), queues no free Dpc, and reschedules for the last time.

## Alternatives considered
None yet.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Lock order between the exiting thread's PI lock, the result wait queue and each joiner's PI lock | To be laid out once the design is drafted: chain-lock backoff as in Zircon, or an order that avoids nesting | None yet | | |
| Who writes the core function | The user writes the joiner side (park the caller, atomic with exit's wake) under a time limit, as the milestone hand-written function in `docs/authored.md`; or the agent writes it | The user, by CLAUDE.md's milestone rule | | |

Per CLAUDE.md, the concurrency decision needs the user's written argument first: which invariant the lock protects, who accesses it concurrently, and the worst interleaving. The agent grades it after.

## Testing and verification
- A kernel test command in `kernel/commands` (`thread.join.test` or similar). It joins a thread that has already exited, a thread that exits while the joiner is parked, and a join that times out on its deadline. It also detaches an already-Dead thread.
- A stress run on SMP: many spawn and join pairs racing exit against park. Run with lockdep on.
- `ours-verify` on both images, and the test run with `scripts/kshell_run.py` on both.

## Open questions
- Whether `TaskState::join()`'s current shape, which parks a given thread on the result queue, survives, or whether the park moves into `Thread::join()` around `park_thread_preamble_locked()`.
- How this interacts with the `wait_queue` work another session has in flight (`kernel/include/ours/task/wait_queue.hpp`, `kernel/kernel/task/wait_queue.cpp`, `kernel/commands/wait_queue_test.cpp`, uncommitted on 2026-10-03).
