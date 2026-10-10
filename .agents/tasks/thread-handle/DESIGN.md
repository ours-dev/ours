# thread-handle: Thread::spawn() returns a unique JoinHandle, and a thread is freed by whichever of two references goes last

**Date**: 2026-10-10
**Status**: Proposed

## Problem
`Thread::spawn()` returns a raw `Thread *`, and the code that frees a thread has to work out who frees it and when that is safe:

- A caller can touch a thread it detached after the thread has freed itself. `wait_queue_test.cpp` resumes a detached waiter it trusts to be alive.
- A joinable thread that nobody joins stays a zombie on `SPAWNED_THREADS` for good. A thread that is never resumed cannot be disposed of at all.
- `exit()` chooses one of three endings under the PI lock and queues one of two Dpc bodies.
- `join()` and `detach()` need the Joined and Detached claims so that exactly one party frees the thread.
- `detach()` of a dead thread blocks inside `join()`.

The user wants `spawn()` to return a handle that owns the thread, the way Rust's `std::thread::JoinHandle` does. A debate between this scheme, the baseline (Zircon) scheme and the reference-counting experiment (Linux, `2aabb13a`) is in `out/reports/2026-10-10-thread-reaping-debate.md`; its judge recommended this scheme.

## Goals and non-goals
Goals:
- `spawn()` returns a move-only `JoinHandle`. Dropping it without a join detaches the thread.
- `join(deadline)` waits for the exit and returns the return code; it consumes the handle on success and keeps it on timeout.
- A thread whose handle is dropped before it was ever resumed is freed then.
- The Joined and Detached flags and the claim logic go away.
- `Thread` grows by no bytes.

Non-goals:
- A copyable handle that lets a third party keep a thread alive. Added only when something needs one.
- Batch join ("join a group of threads"). This design is meant to make it easy later, not to build it.
- ChainLock, and any change to the object layer's reference counting of `ThreadDispatcher`.

## Design
Objects:
- `Thread::m_refs`, a `std::atomic<u32>` that fills existing padding. It counts the handle's reference and the thread's own. `drop_ref()` destroys the thread when it reaches zero.
- The handle's reference: one at `spawn()`, dropped when the `JoinHandle` is dropped or a join succeeds.
- The thread's own reference: added by `resume()` on the Initial branch, under the PI lock, and dropped by a Dpc pinned to the thread's CPU after its final switch. It stands for "a CPU may still run on this stack, or the next thread still holds this thread's PI lock".
- `JoinHandle`: holds the `Thread *`; move-only.

Events, for a thread that is joined:
1. `spawn()` returns a handle; `m_refs` is 1.
2. `resume()` adds the thread's own reference; `m_refs` is 2.
3. `exit()`, under the PI lock, sets Dead and the return code; then it unlinks the thread from `SPAWNED_THREADS`, signals `exited()` outside the PI lock, queues the Dpc and switches away.
4. `join()` wakes, reads the return code and drops the handle's reference.
5. The Dpc drops the thread's own reference. Whichever of 4 and 5 comes second destroys the thread.

A detached thread is the same without step 4: dropping the handle drops its reference at any time. A thread never resumed has only the handle's reference, so dropping the handle destroys it; `destroy()` must then unlink it and undo `Scheduler::on_thread_init()`.

`ThreadDispatcher` holds its kernel thread's `JoinHandle` and drops it in `mark_dead()`, which `on_exit()` calls before the thread is Dead. That breaks the cycle with `Thread::m_user_thread`, which `~Thread` still releases only after the final switch, as `ArchVmAspace::destroy()` and `~IoBitmap` require.

Counterparts:
- Rust: `std::thread::JoinHandle` is not `Clone`, `join()` consumes it, and dropping it detaches.
- Linux: `task_struct::rcu_users` starts at 2 in `dup_task_struct()` (kernel/fork.c), one for the reaper and one for the scheduler; the scheduler's is put in `finish_task_switch()` (kernel/sched/core.c). Ours drops the thread's own reference from a pinned Dpc instead, because `Scheduler::complete_context_switch()` runs with the next thread's PI lock held and `destroy()` takes the dead thread's PI lock and its resource lock, which is never on a scheduler path.
- Zircon: `Thread::Create()` returns a raw `Thread *`; `Thread::Join()` and `Thread::Detach()` hand the free to one party (kernel/kernel/thread.cc). This design departs from it at the kernel-thread API.

## Alternatives considered
- Baseline (Zircon) scheme: keeps the raw pointer, its use-after-free and leaks, the three endings and the blocking `detach()`. It would win if ChainLock landed first, since `exit()` could then wake joiners under its own lock as Zircon does.
- Reference counting alone (`2aabb13a`): keeps the raw pointer and still needs both claims, because someone has to decide who drops the reaper's reference. It is the base this design builds on.
- A copyable handle: any holder's drop may be the last, so the free can happen in any context. Deferred until a third party needs to keep a thread alive.
- ustl's intrusive `RefCounted` and `Rc`: adds 32 bytes and a vptr, its weak count would go unused, the free would still go through `rc_recycle()` into `ThreadCache`, and a const `Rc`'s `operator->` yields a const pointer.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Dropping a handle that was not joined | detach, as Rust; panic, as `std::thread` | detach | None | detach (2026-10-10) |
| When the thread's own reference is taken | at spawn; at the first resume | at the first resume | None | at the first resume (2026-10-10) |
| Unique or copyable handle | unique; copyable | unique | None | unique (2026-10-10) |
| How the count is implemented | ustl `RefCounted` and `Rc`; a hand-written u32 | a hand-written u32 | None | |
| Land `2aabb13a` first | as its own commit; straight to the handle | as its own commit | None | |
| Where the last drop may run | assert preemptible with no spin lock held; defer every free through a Dpc embedded in each thread (about 40 bytes) | assert | None | |
| `exited()` means Dead rather than off its stack | accept; keep the old meaning | accept | None | |
| Recording the departure from Zircon in `ours-kernel-design` | record it, citing Rust for the API and Linux for the exit path; do not record it | record it | None | |
| The function the user writes by hand this milestone | the handle's drop path; `exit()` | the handle's drop path | None | |

## Testing and verification
- `threadjoin.test` rewritten for the handle: a join after the exit, a join that waits, a join that times out and then succeeds, a dropped handle that detaches, a handle dropped before resume that frees the thread (checked through `thread.destroy`), and the stress mode.
- `interruptline.test`, which caught the last premature free, run several times.
- `boot_check.py`: both images boot.
- Not verifiable yet: lockdep does not link, so "no two PI locks nest" and "no spin lock held at the last drop" rest on argument and assertions.

## Open questions
- How a timed-out join hands the handle back: as part of its result, or by taking the handle by reference and consuming it only on success.
- Whether idle and boot threads, which are never resumed through `resume()` and never exit, need anything beyond never getting a handle.
