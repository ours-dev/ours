# object changes

## A refused start no longer touches the thread's entry (2026-10-23)

**Files**
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — `start()` writes nothing; `activate()` saves the entry under the thread's lock
- `kernel/object/process_dispatcher.cpp`, `kernel/object/include/ours/object/process_dispatcher.hpp` — `attach_thread()` carries the entry to `activate()`
- `docs/repairs/261023/object.md` — the item removed

**Symptom**
A program could stop the kernel: `process_start` twice on the same thread, the second before the started thread had run, left that thread reading an empty entry in `ThreadDispatcher::trampoline()` -- `DEBUG_ASSERT(self->m_user_entry)` in the dbg image, a read of an empty `ustl::Option` in the rel image. Found by the review of the task-control design; not seen happen.

**Cause**
`start()` wrote `m_user_entry` and `m_starts_process` as its first act, with no lock and before anything had checked that the thread could still be started, and erased the entry when `attach_thread()` refused. The checks live further down -- the process's state in `attach_thread()`, the thread's in `create_kernel_thread()` -- so a call bound to be refused had already overwritten and then cleared the entry of a thread an earlier call had started. The fields carry no `TA_GUARDED`, so nothing flagged a write to state another thread reads.

The general mistake is the order "write, then validate, then undo on failure" on state that is shared with someone who may read it in between. Undo is only safe on state nobody else can see yet; here the earlier call had already published the thread.

**Change**
Before:

```cpp
m_user_entry = state;
m_starts_process = first_thread;
auto status = m_process->attach_thread(*this, first_thread);
if (Status::Ok != status) {
    m_user_entry = ustl::none();
    return status;
}
```

After, in `start()`:

```cpp
return m_process->attach_thread(*this, first_thread, state);
```

and in `activate()`, the one place a thread leaves Initial, under its lock:

```cpp
DEBUG_ASSERT(m_state == State::Initial && m_kernel_thread != nullptr, ...);
m_user_entry = entry;
m_starts_process = starts_process;
```

A start refused by the process's state or the thread's never reaches `activate()`, so it writes nothing, and there is nothing to undo.

**Reference**
Zircon `ThreadDispatcher::MakeRunnable()` (`kernel/object/thread_dispatcher.cc:164-176`): the lifecycle is checked under the thread lock, and only then is `user_entry_` saved; `ThreadDispatcher::Start()` hands the entry to `ProcessDispatcher::AddInitializedThread()`, which calls `MakeRunnable()` under the process lock, the shape `attach_thread()` now has.

**Verification**
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), 146 host tests pass. Every program the shell starts takes the new path; the launcher, job debugger, `thread_create`, `process_start` and `start()` checks pass.
- Not reproduced: a program cannot today hold a thread that has been started but has not run, so the race has no deterministic test from user space. `.agents/tasks/task-control/` step 6 adds one with `thread_start`, where the shell holds its own new thread.

**Deliberately not done**
`TA_GUARDED` on `m_user_entry`: the trampoline reads it without the lock, ordered by the resume that follows the write, and an annotation would only be silenced there.

**How it was found**
Three independent reviewers of the task-control proposal flagged it from the code; confirmed by reading `start()`, `attach_thread()` and `trampoline()`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why is "write, check, undo on failure" wrong here, when it is fine for a value nobody else can see yet?
2. Which call is the first that can no longer refuse a start, and why is that where the entry must be written?
3. What orders the trampoline's read of the entry after `activate()`'s write, given that the trampoline takes no lock?

## A thread raises ThreadRunning when it starts and Terminated when it has left its process (2026-10-23)

**Files**
- `system/public/ours/signals.hpp` — ThreadRunning (bit 4) and ThreadSuspended (bit 5); Terminated now raised by a thread too
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — `activate()` raises ThreadRunning with the state; which signal goes with which state
- `kernel/object/process_dispatcher.cpp` — `detach_thread()` raises the thread's Terminated, outside the process's lock and before the process's own
- `system/app/shell/shell_tests.cpp` — a thread not started raises nothing; a starting thread reads ThreadRunning alone; a dead thread reads Terminated and not ThreadRunning

**Symptom**
None: a missing interface. A thread raised no object signal at all (`Signals::None`), so a program holding a thread handle could not wait for the thread to start or to end; `signals.hpp` said so ("a thread or a job never does").

**Cause**
Thread signals came with the plan to suspend threads from user space (`.agents/tasks/task-control/`): a program that suspends a thread waits for ThreadSuspended, and one that starts a thread and wants it gone waits for Terminated. Without them every later step would have nothing to wait on.

**Change**
ThreadRunning is raised where the thread leaves Initial, in `activate()`, under the thread's lock it already holds -- so with `update_state_locked()`, since `update_state()` takes that lock and would take it a second time:

```cpp
m_state = State::Running;
update_state_locked(Signals::None, Signals::ThreadRunning);
```

Terminated is raised in `ProcessDispatcher::detach_thread()`, after the process's lock is released and before `finish_dead_transition()`:

```cpp
thread.update_state(Signals::ThreadRunning | Signals::ThreadSuspended, Signals::Terminated);
```

Outside the lock because telling observers takes their own locks; before the process's own Terminated so that a waiter woken by the process finds its threads finished too. ThreadSuspended is defined for the next steps and raised by nothing yet.

**Reference**
Zircon `ZX_THREAD_TERMINATED`, `ZX_THREAD_RUNNING`, `ZX_THREAD_SUSPENDED` (`system/public/zircon/types.h`, object signals 3, 4 and 5), switched by `ThreadDispatcher::SetStateLocked()` (`kernel/object/thread_dispatcher.cc`); a thread is DEAD before `ProcessDispatcher::RemoveThread()` ends the process, the order kept here.

**Verification**
- Shell, both images: a thread not started answers TimeOut to a poll for ThreadRunning or Terminated. dbg image: at ProcessStarting the job debugger reads ThreadRunning alone on the starting thread (three debuggers); each of the 16 faulting probes' threads reads Terminated, and not ThreadRunning, once it is dead. The poll is deterministic: the thread raises Terminated before the process's exception channel closes, which the shell has already seen.
- With `thread_dispatcher.cpp` and `process_dispatcher.cpp` reverted, the dbg image reports "the starting thread does not read Running alone" and "a dead thread does not read Terminated", and the job debugger, exception channel and ioport checks fail.
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
The `ExtraSignals` parameter of `SoloDispatcher`: nothing reads it, and the process, which raises Terminated, declares `Signals::None` there too.

**How it was found**
None: step 5 of `.agents/tasks/task-control/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why must `activate()` use `update_state_locked()`, and what would `update_state()` do there?
2. Why is the thread's Terminated raised outside the process's lock, and why before the process's own?
3. Why can the shell check a dead thread's Terminated with a poll instead of a wait?

## A program can suspend a thread, and resume it by closing the token (2026-10-23)

**Files**
- `kernel/object/include/ours/object/suspend_token_dispatcher.hpp`, `kernel/object/suspend_token_dispatcher.cpp` (new), `kernel/object/BUILD.gn`, `kernel/object/include/ours/object/types.hpp`, `system/public/ours/types.hpp` — the suspend token, `ObjectType::SuspendToken = 27`
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — the suspend count, the Suspended state, real `suspend()`/`resume()`, `on_suspend()`/`on_resume()` switching state and signals, `activate()` reading the count, `kill()` and `on_exit()` taking Suspended, the exception path asking `is_leaving_locked()`
- `kernel/object/process_dispatcher.cpp` — `attach_thread()` no longer passes a suspend flag
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/task.cpp`, `kernel/lib/syscall/BUILD.gn` — `task_suspend`, in place of the `thread_suspend` placeholder
- `system/app/shell/shell_tests.cpp` — `check_task_suspend()`

**Symptom**
None: a missing interface. The kernel could suspend a thread, but nothing a program could call reached it: `ThreadDispatcher::suspend()` and `resume()` only checked the canary, and the thread had no Suspended state or signal.

**Cause**
The pieces it needed came first: an Initial thread that meets its signal (`docs/changes/261023/kernel-task.md`), the vDSO retry and the fault-path translation that keep InternalIrqRetry from a program (`docs/changes/261023/abi.md`, `kernel-task.md`), the thread's object signals, and a second thread to suspend (`docs/changes/261023/syscall.md`).

**Change**
Objects, in the order a suspend meets them:

- `task_suspend(task, &token)` resolves the thread with Write; a process with Write answers NotSupported (the user's D3), anything else BadType.
- `SuspendTokenDispatcher::create()` refuses the calling thread (NotSupported), makes the token first so that running out of memory suspends nothing, then asks the thread to suspend, and keeps the thread only if that was granted.
- `ThreadDispatcher::suspend()` refuses Dying and Dead (BadState), counts, and only the count's step from 0 to 1 reaches the kernel thread; an Initial thread is only counted, and `activate()` reads the count under the same lock that moves it out of Initial, so it starts suspended.
- The kernel thread stops at its way back to ring 3; `on_suspend()` makes it Suspended and swaps ThreadRunning for ThreadSuspended, under one hold of the lock, so an observer woken by the signal finds the state.
- Closing the token's last handle runs its `on_zero_handles()`, which takes the thread out under the token's lock and calls `resume()` after letting it go -- a ThreadDispatcher's lock comes before any other dispatcher's. The count's step from 1 to 0 resumes the kernel thread; `on_resume()` swaps the signals back. A thread Dying or Dead is left alone.

```cpp
m_suspend_count += 1;
switch (m_state) {
    case State::Initial:
        break;
    case State::Running:
    case State::Suspended:
        if (1 == m_suspend_count) {
            auto _ = m_kernel_thread->suspend();
        }
        break;
    ...
}
```

Suspended as well as Running reaches the kernel thread, because the last token may just have gone: its `resume()` has reached the kernel thread but the thread has not yet run `on_resume()`, and a new token is a new hold. The mirror case is in `resume()`: Running as well as Suspended, because the thread may not have reached `on_suspend()` yet when the last token goes.

`kill()` treats Suspended as Running: the kill unparks the thread and it dies where it stopped. The exception path read `m_state != Running` as "killed", which would now also catch Suspended; it asks `is_leaving_locked()` -- Dying or Dead -- instead.

**Reference**
Zircon `zx_task_suspend` (`kernel/lib/syscalls/task.rs`, `ZX_RIGHT_WRITE`), `SuspendTokenDispatcher` (`kernel/object/suspend_token_dispatcher.rs`; `ZX_DEFAULT_SUSPEND_TOKEN_RIGHTS` is TRANSFER and INSPECT), `ThreadDispatcher::Suspend()`, `Resume()`, `Suspending()`, `Resuming()`, `MakeRunnable()` and `SetStateLocked()` (`kernel/object/thread_dispatcher.cc:165-302, 405-441, 486-513`). Two differences: Zircon counts a suspend of a Dying thread before refusing it, and this does not, so the count is always the number of live tokens; and Zircon's `zx_task_suspend` also takes a process, which this answers NotSupported for now.

**Verification**
- Shell, both images (`task_suspend checks passed`): BadHandle, BadType for a VMA, NotSupported for this process, AccessDenied without Write, NotSupported when a second thread asks to suspend itself, BadState for an ended thread, AccessDenied for duplicating a token. A thread suspended before it is started reads ThreadSuspended and has counted nothing; released, it counts. A thread counting in ring 3, suspended, reads ThreadSuspended and its count stands still; a second token keeps it stopped after the first is closed; closing the last lets it count again. A thread parked in `port_wait`, suspended and resumed, then receives the packet queued for it with Ok.
- That last case exercised the retry: with a temporary log line in `PortDispatcher::dequeue()` (not committed), the dbg boot showed `port_wait interrupted: InternalIrqRetry` during the check, and the thread's program still saw Ok and its packet.
- `ours-verify`: dbg PASS (41 passed, 4 skipped), rel PASS (39 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
Suspending a process (D3); `task_kill`, which is step 8; a thread-state query, without which the shell cannot tell for certain that a suspend landed while the thread was parked rather than just before (the log above shows it did in this run).

**How it was found**
None: step 7 of `.agents/tasks/task-control/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why does `suspend()` reach the kernel thread when the dispatcher is already Suspended, and `resume()` when it is still Running?
2. Why does the token resume the thread only after letting go of its own lock?
3. How does a thread suspended before it starts end up stopped before its first instruction, step by step?
