# thread-join: make Thread::join() work again, as Zircon's Thread::Join() does

**Date**: 2026-10-03
**Status**: Accepted

On 2026-10-10 a replacement, `.agents/tasks/thread-handle/DESIGN.md` (a `JoinHandle` from `spawn()` and freeing by the last of two references), was implemented, merged and then abandoned and reverted. This design stands.

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
Option B of the lock-order decision (decided 2026-10-04): no code path ever holds two threads' PI locks at once. The joiners do not wait for "T is Dead". They wait for "T is off its stack", and a Dpc publishes that after T's last context switch.

Objects:
- `TaskState::m_exited`: a manual-reset `sync::Event`, replacing `m_result_wait_queue`. It is signalled once, with `Ok`, after T has switched out for the last time. `event.hpp` includes only `wait_queue.hpp`, so `thread.hpp` can hold one without an include cycle.
- T's PI lock: guards `state()` (Dead) and `m_flags`. It gains `Joined`, the claim of the one thread allowed to reap T. T holds this lock from setting Dead until its final switch completes; the next thread releases it in `Scheduler::complete_context_switch()` (`kernel/kernel/task/sched/scheduler.cpp:591-595`).
- The exit Dpc: queued by `exit()` for a joinable T, as the detached path already queues its free Dpc, and on T's stack like that one. The DPC worker that runs it is pinned to T's CPU (`kernel/kernel/task/dpc.cpp:34`), and T keeps preemption off until its final switch, so the Dpc runs only after T has left its stack. `DpcRunner` does not touch a Dpc after invoking it (`dpc.cpp:99-108`).
- `SPAWNED_THREADS` and `SPAWNED_THREADS_LOCK`: the reaper unlinks T, as Zircon's `Join()` calls `EraseFromListsLocked()`.

Ownership rule: at any moment exactly one party may free T. That is T itself through the free Dpc if T is Detached, or the joiner that holds the `Joined` claim. `Detached` and `Joined` are never set together.

Exit side, joinable T:
1. Under T's PI lock: set Dead and the return code; T is not Detached, so it is not unlinked and no free Dpc is queued.
2. Drop the PI lock, queue the exit Dpc, retake the PI lock, final reschedule. The gap is safe for the reason the detached path's is: the Dpc cannot run before the switch, and nothing frees T before the Dpc signals.
3. The Dpc, on T's CPU after the switch: take and release T's PI lock (it is free only once the switch has completed), then `m_exited.signal(Ok)`. It wakes the joiner with no PI lock held.

Joiner side, `Thread::join(retcode, deadline)` (the user's hand-written function):
1. `DEBUG_ASSERT(this != current)`.
2. Under T's PI lock: `Detached` or `Joined` already set means `BadState`; otherwise set `Joined`.
3. Wait on `m_exited` with the deadline, not interruptible (Zircon's `TaskState::Join()` blocks `Interruptible::No`). `Event::wait_etc()` already makes "is it signalled?" and the enqueue one step under the queue lock.
4. On a timeout: under T's PI lock, clear `Joined`, return `TimeOut`. Safe: a claimed T is freed by nobody else.
5. On `Ok`: read the return code, unlink T from `SPAWNED_THREADS`, `destroy()` it.

`detach()`:
- Under T's PI lock: `Joined` set means `BadState` (see the decision below). Not Dead: set `Detached`, done.
- Dead and joinable: drop the PI lock, then `join(nullptr, infinite)`. Zircon's `Detach()` drops its lock before joining too; the old body here joined with the lock held.

Removed: `TaskState::join()`, `TaskState::try_wake_joiners()`, the `notify_all()` in `TaskState::invoke()` (it woke joiners before T was even Dead), and `exit()`'s panic on a joinable thread.

Locks: T's PI lock is only ever held alone, or outside `SPAWNED_THREADS_LOCK` and the event's queue lock, which is the existing order. The Dpc and the joiner each hold one PI lock at a time.

## Alternatives considered
- **A. Zircon's shape with a local backoff.** The joined thread's PI lock is taken before the joiner's. The joiner takes its own with `SpinLock::try_acquire()` and drops everything and retries on failure, and `exit()` wakes the joiners inline under its own PI lock. Rejected: it nests two locks of one class with an ad hoc protocol that the planned ChainLock (`.agents/skills/ours-kernel-design/references/task.md`) would replace; mutual joins livelock; and whether this lockdep accepts same-class nesting was not established. Zircon's own TODO in `Thread::Current::Exit()` (`zircon/kernel/kernel/thread.cc:1067-1093`) prefers dropping the thread's lock to wake joiners unconditionally, with a cleanup lock guarding the stack; the exit Dpc plays that role here.
- **A2. A fixed order with no backoff.** As A, but joined before joiner always. Rejected: a join cycle spins two CPUs for good with interrupts off, where Zircon at worst leaves two threads blocked.
- **Reference counting, as Linux's `finish_task_switch()` drops the last reference to a dead task.** Rejected: it needs a hook after the switch in `scheduler.cpp`, generic scheduler machinery the user is to be asked about first, and it changes the thread lifetime model the rest of the kernel assumes.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Lock order between the exiting thread's PI lock, the joiners' wake and each joiner's PI lock | A: Zircon's order with a local try-lock backoff; B: never nest two PI locks, a Dpc signals an Event after the final switch | B | B; the written argument is pending | B, 2026-10-04 |
| `detach()` while a joiner holds the claim | Zircon: `Detach()` wakes the joiners with `ZX_ERR_BAD_STATE`; here: refuse the detach with `BadState` while `Joined` is set | Refuse: with B, waking the joiner from `detach()` either nests PI locks or signals an event T may already have freed, and a timed-out joiner's un-claim would race T freeing itself | Refuse, as recommended | Refuse, 2026-10-04 |
| Who writes the core function | The user writes the joiner side under a time limit, as the milestone hand-written function in `docs/authored.md`; or the agent writes it | The user, by CLAUDE.md's milestone rule | | The agent, at the user's request, 2026-10-04; nothing is recorded in `docs/authored.md`, and the written lock-order argument was not made |

Per CLAUDE.md, the concurrency decision needs the user's written argument first: which invariant the lock protects, who accesses it concurrently, and the worst interleaving. The agent grades it after.

## Testing and verification
- A kernel test command in `kernel/commands` (`thread.join.test` or similar). It joins a thread that has already exited, a thread that exits while the joiner is parked, and a join that times out on its deadline. It also detaches an already-Dead thread.
- A stress run on SMP: many spawn and join pairs racing exit against park (`threadjoin.test stress ROUNDS`). Lockdep cannot be on for it: a `lockdep_enable = true` build does not link, `docs/repairs/260917/kernel-task-sched.md` item 6, a feature of its own; the stress runs on dbg_1 with the PI-lock assertions only.
- `ours-verify` on both images, and the test run with `scripts/kshell_run.py` on both.

## Open questions
- `sync::Event` exposes a deadline wait only as interruptible (`Event::wait(deadline)`); the join needs a non-interruptible one, so `Event` needs a public form of `wait_etc()`'s choice.
- The `kshell_run.py` runs of the test can only happen on dbg_1: the rel image has booted with `kernel.shell=false` since `ead5d12f`.
- Whether `destroy()` should take over the `SPAWNED_THREADS` unlink for both paths, rather than the reaper doing it.
