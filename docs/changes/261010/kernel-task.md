# kernel-task changes

## Title: Thread::spawn() returns the thread's one JoinHandle, and the Joined and Detached flags are gone (2026-10-10)

**Files**
- `kernel/include/ours/task/thread.hpp` — `JoinHandle` (move-only; `join()`, `detach()`, `operator->`, `get()`); `spawn()` returns it; `Thread::join()` and `Thread::detach()` removed; `Flags::Detached` and `Flags::Joined` removed; `m_refs` starts at one
- `kernel/kernel/task/thread.cpp` — `JoinHandle`'s members; `resume()` takes the thread's own reference on the way out of Initial; `destroy()` unlinks a thread that never ran; `drop_ref()` asserts that the last drop can block; `exit()` unlinks every thread itself and always signals `exited()`; `bind_user_thread()` no longer detaches
- `kernel/include/ours/dpc.hpp`, `kernel/kernel/task/dpc.cpp` — a Dpc queue keeps its worker's handle
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp` — `ThreadDispatcher` holds its kernel thread's handle and gives it up in `mark_dead()`
- `kernel/lib/kshell/kshell/shell.cpp`, `kernel/top/main.cpp` — resume, then let the handle go
- `kernel/commands/thread_join_test.cpp` — rewritten against the handle's contract
- `kernel/commands/{interrupt_line,interrupt,ioport,irq,lockup,mutex_stress,port,semaphore,sleep,vmo_snapshot,wait_queue}_test.cpp` and `lockup.cpp` — migrated to the handle

**Symptom**
- `spawn()` handed out a raw `Thread *`, and the caller had to remember to call exactly one of `join()` and `detach()`.
- `sleep_test.cpp` and `wait_queue_test.cpp` kept suspending, resuming and reading threads they had already detached, on the promise that those threads stayed alive.
- A thread that was never resumed had no way to be freed.
- A thread whose join timed out and that nobody detached leaked.

**Cause**
The raw pointer separated owning a thread from using it. After a `detach()`, any copy of the pointer could outlive the thread. Freeing still needed two claim flags to agree on which single party would free the thread.

**Change**
- **The handle.** `spawn()` returns a `JoinHandle`, the thread's only owner. It is move-only, so a thread has at most one joiner.
- **References.** The handle holds one of the thread's references. The thread's first `resume()` adds a second, its own, which a Dpc pinned to its CPU drops after its final switch. Whichever goes last frees the thread.
- **Join and detach.** `join()` waits on `exited()`, reads the return code and gives up the handle. A timed-out join keeps the handle, so the caller can join again. Dropping a handle that has not joined detaches the thread.
- **A thread that never ran.** It has only its handle's reference, so dropping the handle frees it: `destroy()` unlinks it, because it never reached the unlinking in `exit()`.
- **No reaper.** The claim logic is gone with the reaper: `exit()` unlinks every thread itself, and nobody frees anyone; the last reference out does.
- **Where the last drop may run.** It may destroy the thread, and `~Thread` releases things that can block, so `drop_ref()` asserts interrupts and preemption are on.
- **`ThreadDispatcher`.** It holds its kernel thread's handle and gives it up in `mark_dead()`, on the thread's way out. That is never the last reference, so `~Thread` still releases the process's last reference only after the final switch, as `ArchVmAspace::destroy()` requires.
- **Call sites.** Every caller resumes through the handle before letting it go. Tests that used a detached thread afterwards now hold its handle until they are done with it.

**Reference**
- **Rust.** The handle follows `std::thread::JoinHandle`: it is not `Clone`, `join()` consumes it, and dropping it detaches.
- **Linux.** The two references follow `task_struct::rcu_users`, which `dup_task_struct()` starts at 2 (kernel/fork.c). The scheduler's reference is put in `finish_task_switch()` (kernel/sched/core.c); ours drops the thread's own reference from a pinned Dpc instead, for the reasons in the 2026-10-09 entry.
- **Zircon.** `Thread::Create()` returns a raw `Thread *` (kernel/kernel/thread.cc); this departs from it at the kernel-thread API.
- **Design.** `.agents/tasks/thread-handle/DESIGN.md`. The debate that chose it is in `out/reports/2026-10-10-thread-reaping-debate.md`.

**Verification**
- **Build and boot.** `boot_check.py`: build ok; dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 8 skipped, 6/6 required), host tests PASS (144).
- **Thread tests**, through `kshell_run.py` on dbg:
  - `threadjoin.test stress 2000`: ok, 13 checks; 2000 rounds of 4 threads, half joined and half detached, 0 bad.
  - `threadjoin.test stress 1000`: ok twice.
  - `interruptline.test`: ok (58 checks) in 3 of 3 runs.
- **Other commands**, once each: `interrupt.test` (32), `ioport.test` (14), `irq.test` (72), `port.test`, `semaphore.test` (18), `sleep.test` (10), `vmosnapshot.test` (205), `waitqueue.test` (4), `lockup spin 1 50` and `mutex.stress 2000` all report ok.
- **The `mutex.stress` default.** With its default 20000 rounds it called itself STUCK. That is the known slow-run report in `docs/repairs/261003/kernel-commands.md` item 1.
- **clang-tidy.** On the 17 changed `.cpp` files against `out/bringup.x86`: no finding on a changed line.

**Deliberately not done**
- A copyable handle, for a party that only watches a thread: added when one is needed.
- `Thread::detach_and_resume()`, declared and never defined, is left alone.
- `.agents/tasks/thread-join/DESIGN.md` still describes the claim flags; `.agents/tasks/thread-handle/DESIGN.md` supersedes it once this lands.
- `destroy()` still reads NoFree, which no thread that reaches it has.

**How it was found**
The user wanted `spawn()` to return a handle with a reference count instead of a raw pointer, chose Rust's semantics and a unique handle, and asked for a version to look at.

**The user's prediction**
None yet.

**Three questions you should be able to answer**
- Why does the thread's own reference come from `resume()` and not from `spawn()`?
- Why must a caller resume a thread through its handle before letting the handle go?
- Why is `ThreadDispatcher::mark_dead()` never the last reference to its kernel thread?

## Title: spawn() returns an UnstartedThread, whose start() is the only ordinary way to a JoinHandle, and join() says what its return does not promise (2026-10-10)

**Files**
- `kernel/include/ours/task/thread.hpp` — `UnstartedThread` (move-only, `[[nodiscard]]`; `start()`, `defer_start()` and `cancel()`, each consuming it); `spawn()` returns it; `JoinHandle::join()` documents that its return does not mean the thread is off its CPU
- `kernel/kernel/task/thread.cpp` — `UnstartedThread`'s members; a debug build stops on one dropped with none of the three
- `kernel/object/thread_dispatcher.cpp` — `create_kernel_thread()` holds its thread through `defer_start()`, and `activate()` resumes it as before
- `kernel/kernel/task/dpc.cpp`, `kernel/lib/kshell/kshell/shell.cpp`, `kernel/top/main.cpp`, `kernel/commands/lockup.cpp` and the test commands — every spawn site starts through `start()`
- `kernel/commands/thread_join_test.cpp` — the "dropped before resume" check becomes a `cancel()` check and a `defer_start()` check

**Symptom**
With `spawn()` returning a `JoinHandle`, three mistakes compiled:
- **Detaching before resuming:** `handle.detach(); handle->resume();` freed the thread unrun, then stopped a debug build on the empty handle.
- **Joining before resuming:** `handle.join()` waited for good.
- **Never starting it:** dropping the handle without a resume freed the thread unrun with no report at all. This is the kind of omission that once left the console thread parked for good (`docs/changes/260910/kshell.md`).

**Cause**
One type stood for two states with different legal operations. Before its start a thread may only be configured, started or given up; after it, it may be joined or detached. The `JoinHandle` offered all of these in both states.

**Change**
- **The unstarted state is its own type.** `spawn()` returns an `UnstartedThread`. It can be configured through `operator->`, and three operations end it:
  - `start()` resumes the thread and returns its `JoinHandle`;
  - `defer_start()` returns the handle without resuming, so its owner's first `resume()` is the start;
  - `cancel()` frees the thread unrun.
- **The type rejects the first two mistakes.** `join()` and `detach()` exist only on `JoinHandle`, so joining or detaching an unstarted thread no longer compiles.
- **Dropping it unended is reported.** An `UnstartedThread` dropped without any of the three stops a debug build at the drop, and is freed unrun in every build. That makes the third mistake loud.
- **Each transition consumes the old state.** Each is rvalue-qualified (`ustl::move(thread).start()`), the same shape as `ustl::Result::unwrap()`. `start()` is `[[nodiscard]]`, so a thread nobody joins is let go explicitly, as in `ustl::move(thread).start().detach()`.
- **ThreadDispatcher defers its start.** It holds its kernel thread from `create_kernel_thread()` to `activate()`, in Zircon's way, so it uses `defer_start()`.
- **`join()` documents what it does not promise.** A return does not mean the thread is off its CPU: the thread signals `exited()` before its final switch. So a caller must not free what that switch still uses, the thread's address space above all, on the strength of a join. No caller does: ioport.test's observer switches off the test's address space itself before it reports done.

**Reference**
- **Typestate.** Encoding an object's states as separate types, so that only the legal operations of each state compile, is the typestate pattern. Rust's `std::thread::Builder` gets there by starting a thread at `spawn()`; this kernel keeps a configure-then-start step, because callers set affinity and an address space before a thread runs.
- **Deferred start.** `defer_start()` matches how Zircon's `ThreadDispatcher` holds `core_thread_` from creation and decides at start whether to `Resume()` or `Suspend()` it (`object/thread_dispatcher.cc`).

**Verification**
- **Build and boot.** `boot_check.py`: build ok; dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 8 skipped, 6/6 required), host tests PASS (144).
- **Thread tests.** `threadjoin.test stress 2000`: ok, 15 checks, 0 bad. `interruptline.test`: ok in 3 of 3 runs.
- **Other commands.** `interrupt.test`, `ioport.test`, `irq.test`, `port.test`, `semaphore.test`, `sleep.test`, `vmosnapshot.test`, `waitqueue.test`, `lockup spin 1 50` and `mutex.stress 2000` all report ok.
- **clang-tidy.** No finding on a changed line of the 17 changed `.cpp` files.
- **Not run:**
  - The debug stop on a dropped `UnstartedThread`, which would end the boot.
  - `lockup spin all`, whose spawn loop now fills an `UnstartedThread` array. It spins every CPU until an NMI from outside.

**Deliberately not done**
- A join that waits until the thread is off its CPU. Nothing needs one yet; the thread's own reference is where it would come from.
- Restricting `UnstartedThread::operator->` to the configuring calls. `resume()` through it bypasses `start()`, and the debug stop then reports the drop; the reference count keeps that from leaking or freeing a running thread.

**How it was found**
Reviewing the JoinHandle version, the user agreed to close the "resume before letting the handle go" rule with a type, and to write down what a join no longer promises.

**The user's prediction**
None yet.

**Three questions you should be able to answer**
- Which mistakes does the `UnstartedThread` type turn into compile errors, and which one only into a debug stop?
- Why does `ThreadDispatcher` need `defer_start()` rather than `start()`?
- Why can a caller not free a joined thread's address space as soon as `join()` returns?
