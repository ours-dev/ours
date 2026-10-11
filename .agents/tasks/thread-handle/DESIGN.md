# thread-handle: Thread::spawn() returns an UnstartedThread whose start() gives a unique JoinHandle, and a thread is freed by whichever of two references goes last

**Date**: 2026-10-10
**Status**: Abandoned

Abandoned on 2026-10-10, the user's call, after it was implemented and merged (`9f78ed1e`, `7de7af71`, `e9e44a50`): it added kernel code for an interface that only kernel threads use, and user space gains nothing from it, since user threads go through `ThreadDispatcher` and object handles. Ownership semantics like these belong in a user-space thread library built on thread handles. The three commits were reverted; kernel threads are back on `.agents/tasks/thread-join/DESIGN.md`. The rest of this document is kept as the record of what was built and why.

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
- `spawn()` returns an `UnstartedThread`, which may only be configured and then started, deferred or cancelled; `start()` gives a move-only `JoinHandle`. Dropping the handle without a join detaches the thread.
- `join(deadline)` waits for the exit and returns the return code; it consumes the handle on success and keeps it on timeout.
- A thread that is never started is freed unrun, and forgetting to start one is reported.
- The Joined and Detached flags and the claim logic go away.
- `Thread` grows by no bytes.

Non-goals:
- A copyable handle that lets a third party keep a thread alive. Added only when something needs one.
- Batch join ("join a group of threads"). This design is meant to make it easy later, not to build it.
- ChainLock, and any change to the object layer's reference counting of `ThreadDispatcher`.

## Design
Objects:
- `Thread::m_refs`, a `std::atomic<u32>` that fills existing padding. It counts the handle's reference and the thread's own. `drop_ref()` destroys the thread when it reaches zero.
- `UnstartedThread`: what `spawn()` returns, move-only and `[[nodiscard]]`. It holds the reference `spawn()` made and offers only `operator->`, to configure the thread, and three rvalue-qualified exits: `start()` resumes the thread and passes the reference to its `JoinHandle`; `defer_start()` passes it on without resuming, so the handle owner's first `resume()` is the start; `cancel()` drops it, freeing the thread unrun. Dropped with none of them, it stops a debug build and is freed unrun in any build.
- The handle's reference: the one `spawn()` made, dropped when the `JoinHandle` is dropped or a join succeeds.
- The thread's own reference: added by `resume()` on the Initial branch, under the PI lock, and dropped by a Dpc pinned to the thread's CPU after its final switch. It stands for "a CPU may still run on this stack, or the next thread still holds this thread's PI lock".
- `JoinHandle`: holds the `Thread *`; move-only. `join()` takes it by reference and empties it only on success, so a timed-out join keeps the thread.

Events, for a thread that is joined:
1. `spawn()` returns an `UnstartedThread`; `m_refs` is 1.
2. `start()` resumes the thread, which adds its own reference, and returns the `JoinHandle`; `m_refs` is 2.
3. `exit()`, under the PI lock, sets Dead and the return code; then it unlinks the thread from `SPAWNED_THREADS`, signals `exited()` outside the PI lock, queues the Dpc and switches away.
4. `join()` wakes, reads the return code and drops the handle's reference.
5. The Dpc drops the thread's own reference. Whichever of 4 and 5 comes second destroys the thread.

A detached thread is the same without step 4: dropping the handle drops its reference at any time. A thread never resumed has only the reference `spawn()` made, so `cancel()`, or dropping a deferred handle, destroys it; `destroy()` then unlinks it, and there is nothing to undo of `Scheduler::on_thread_init()`, which only bound it to a discipline.

A join's return means the thread is Dead, not that it is off its CPU: `exited()` is signalled before the final switch. Nothing the final switch uses, the thread's address space above all, may be freed on the strength of a join.

`ThreadDispatcher` takes its kernel thread through `defer_start()` in `create_kernel_thread()`, starts it with a `resume()` in `activate()`, and drops the handle in `mark_dead()`, which `on_exit()` calls before the thread is Dead. That breaks the cycle with `Thread::m_user_thread`, which `~Thread` still releases only after the final switch, as `ArchVmAspace::destroy()` and `~IoBitmap` require.

Counterparts:
- Rust: `std::thread::JoinHandle` is not `Clone`, `join()` consumes it, and dropping it detaches.
- Linux: `task_struct::rcu_users` starts at 2 in `dup_task_struct()` (kernel/fork.c), one for the reaper and one for the scheduler; the scheduler's is put in `finish_task_switch()` (kernel/sched/core.c). Ours drops the thread's own reference from a pinned Dpc instead, because `Scheduler::complete_context_switch()` runs with the next thread's PI lock held and `destroy()` takes the dead thread's PI lock and its resource lock, which is never on a scheduler path.
- Zircon: `Thread::Create()` returns a raw `Thread *`; `Thread::Join()` and `Thread::Detach()` hand the free to one party (kernel/kernel/thread.cc). This design departs from it at the kernel-thread API. `defer_start()` matches how Zircon's `ThreadDispatcher` holds `core_thread_` from creation and decides at start whether to `Resume()` or `Suspend()` it.
- Typestate: the unstarted and started states are separate types, so only each state's legal operations compile. Rust's `std::thread::Builder` gets there by starting a thread at `spawn()`; ours keeps a configure-then-start step, because callers set affinity and an address space first.

## Alternatives considered
- Baseline (Zircon) scheme: keeps the raw pointer, its use-after-free and leaks, the three endings and the blocking `detach()`. It would win if ChainLock landed first, since `exit()` could then wake joiners under its own lock as Zircon does.
- Reference counting alone (`2aabb13a`): keeps the raw pointer and still needs both claims, because someone has to decide who drops the reaper's reference. It is the base this design builds on.
- A copyable handle: any holder's drop may be the last, so the free can happen in any context. Deferred until a third party needs to keep a thread alive.
- ustl's intrusive `RefCounted` and `Rc`: adds 32 bytes and a vptr, its weak count would go unused, the free would still go through `rc_recycle()` into `ThreadCache`, and a const `Rc`'s `operator->` yields a const pointer.
- A `JoinHandle` straight from `spawn()`, with "resume before letting go" as a rule callers keep: a handle detached or joined before the resume either freed the thread unrun or waited for good, and a forgotten resume freed it without a word.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Dropping a handle that was not joined | detach, as Rust; panic, as `std::thread` | detach | None | detach (2026-10-10) |
| When the thread's own reference is taken | at spawn; at the first resume | at the first resume | None | at the first resume (2026-10-10) |
| Unique or copyable handle | unique; copyable | unique | None | unique (2026-10-10); a copyable watcher only once a caller needs one |
| How the count is implemented | ustl `RefCounted` and `Rc`; a hand-written u32 | a hand-written u32 | None | a hand-written u32, as built; accepted with the merge (2026-10-10) |
| Land `2aabb13a` first | as its own commit; straight to the handle | as its own commit | None | as its own commit, in the same merge (2026-10-10) |
| Where the last drop may run | assert preemptible with no spin lock held; defer every free through a Dpc embedded in each thread (about 40 bytes) | assert | None | assert interrupts and preemption on, as built; accepted with the merge (2026-10-10) |
| `exited()` means Dead rather than off its stack | accept; keep the old meaning | accept | None | accept, with `join()` documenting what its return does not promise (2026-10-10) |
| Guarding "start before letting go" | a runtime rule; spawn starts at once; an `UnstartedThread` type | an `UnstartedThread` type | None | an `UnstartedThread` type (2026-10-10) |
| How a timed-out join keeps the thread | a result carrying the handle back; the handle by reference, emptied only on success | by reference | None | by reference, as built; accepted with the merge (2026-10-10) |
| Recording the departure from Zircon in `ours-kernel-design` | record it, citing Rust for the API and Linux for the exit path; do not record it | record it | None | recorded in `references/task.md` (2026-10-10) |
| The function the user writes by hand this milestone | the handle's drop path; `exit()` | the handle's drop path | None | none: the design was abandoned (2026-10-10) |
| Keep the design | keep it; revert to the thread-join design | keep it | None | revert (2026-10-10): kernel complexity for an interface only kernel threads use; RAII handles belong in user space |

## Testing and verification
- `threadjoin.test` rewritten for the handle: a join after the exit, a join that waits, a join that times out and then succeeds, a dropped handle that detaches, a handle dropped before resume that frees the thread (checked through `thread.destroy`), and the stress mode.
- `interruptline.test`, which caught the last premature free, run several times.
- `boot_check.py`: both images boot.
- Not verifiable yet: lockdep does not link, so "no two PI locks nest" and "no spin lock held at the last drop" rest on argument and assertions.

## Open questions
None: the design was abandoned.
