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
