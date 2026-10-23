# syscall changes

## A program can start a later thread of its process, and a thread can end itself (2026-10-23)

**Files**
- `system/public/ours/syscalls.gni` — `thread_start` and `thread_exit`; `thread_create`'s doc no longer says a thread of a running process cannot be started
- `kernel/lib/syscall/src/task.cpp`, `kernel/lib/syscall/BUILD.gn` — the two handlers, in place of their placeholders
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — a thread that exits by its own hand passes Dying (`on_exit()`)
- `system/app/shell/shell_tests.cpp` — `check_thread_start()`: a second thread of the shell, on a stack it maps, reporting back on a channel
- `.agents/tasks/user-pager/PLAN.md` — its step 8 is this

**Symptom**
None: a missing interface. A process had exactly one thread: `thread_create` made threads that only `process_start` could start, and only as a process's first, and a thread could end only with its process. Every later step of `.agents/tasks/task-control/` needs a second thread, to be the one suspended while another watches.

**Cause**
`thread_start` and `thread_exit` were placeholders with no `syscalls.gni` entry, waiting for their preconditions (the design's P1 row: a TLB shootdown and the VMO lock on the fault path), which are met.

**Change**
`thread_start(thread, entry, stack, arg1, arg2)` resolves the thread with ManageThread and calls `ThreadDispatcher::start(..., false)`, which goes through `ProcessDispatcher::attach_thread()` with the process required to be Running; `thread_exit()` is `ThreadDispatcher::exit_current()`, and is `noreturn`. The thread's entry gets `arg1` in %rdi and `arg2` in %rsi; it has no return address, so its code must end in `thread_exit`.

With them, the user's decision D7: a thread that leaves by its own hand passes Dying, as a killed one already did. `on_exit()`, which every exiting user thread runs, makes a Running thread Dying before anything else:

```cpp
{
    Guard guard{get_lock()};
    if (m_state == State::Running) {
        m_state = State::Dying;
    }
}
```

From there the thread will not run in ring 3 again, so a suspend asked of it (task-control step 7) must be refused rather than handed a token whose stop never comes.

**Reference**
Zircon `sys_thread_start_regs()` (`kernel/lib/syscalls/thread.rs`: `ZX_RIGHT_MANAGE_THREAD`, then `thread.start(..., false)`), `sys_thread_exit()` and `ThreadDispatcher::ExitingCurrent()` (`kernel/object/thread_dispatcher.cc:350-359`), which sets DYING before the thread goes. Zircon's `zx_thread_start` doc names ZX_RIGHT_WRITE, but its code checks MANAGE_THREAD; the code is what this follows. Zircon's raw call also takes a thread pointer and an ABI register (`thread_start_regs`); ours takes the five arguments its program-facing call has, and a thread pointer can come behind the same call when thread-local storage needs it (`.agents/tasks/task-control/DESIGN.md`, open question).

**Verification**
- Shell, both images (`thread_start checks passed`): BadHandle, BadType for a channel, AccessDenied without ManageThread, InvArgs for an entry outside user space; the start, after which the second thread reports both its arguments on the channel and its handle reads Terminated and not ThreadRunning; a second start at once and a start of the ended thread are BadState.
- The second start at once is the case `docs/changes/261023/object.md` could not test: the thread has most likely not run when it is made, and before that fix it would have cleared the thread's entry.
- `ours-verify`: dbg PASS (40 passed, 4 skipped), rel PASS (38 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not observable yet: the Dying a voluntary exit passes; a suspend refused for it is checked at step 7.

**Deliberately not done**
A thread pointer argument and the `thread_start_regs` shape; `thread_read_state` and `thread_write_state`.

**How it was found**
None: step 6 of `.agents/tasks/task-control/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why must a thread started with `thread_start` end in `thread_exit` instead of returning from its entry?
2. What decides whether `thread_start` or `process_start` starts a given thread, and where in the kernel is that checked?
3. Why does a thread exiting by its own hand pass Dying, and what would go wrong with a suspend if it went straight from Running to Dead?
