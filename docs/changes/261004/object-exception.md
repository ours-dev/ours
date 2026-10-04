# object-exception changes

## Title: ExceptionDispatcher and Exceptionate, a process's exception slot, and an uncharged handle (2026-10-04)

**Files**
- `kernel/object/include/ours/object/exception_dispatcher.hpp`, `kernel/object/exception_dispatcher.cpp` — new: one exception in flight.
- `kernel/object/include/ours/object/exceptionate.hpp`, `kernel/object/exceptionate.cpp` — new: the kernel's end of an exception channel.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — a process holds an Exceptionate, shut down first as it dies.
- `kernel/object/include/ours/object/handle.hpp`, `kernel/object/handle.cpp` — `Handle::make_uncharged()`.
- `kernel/object/include/ours/object/types.hpp`, `kernel/object/BUILD.gn` — the new class and files.
- `.agents/tasks/exception-channels/PLAN.md` — step 3 done.

**Symptom**
None: nothing creates an exception or binds a channel yet. Step 3 of `.agents/tasks/exception-channels/`.

**Cause**
Exception channels need two kinds of state that did not exist. One is per exception: the faulting thread, the type, what the handler decided, and something for the thread to wait on until the handler closes its handle. That is an object user space holds a handle to, so it is a dispatcher. The other is per task: the kernel's end of the channel a supervisor bound, and whether the task is past binding one. Nobody holds a handle to that, so it is a plain member of the task.

The second has a lifetime trap. An exception message nobody has read holds the exception's handle, the exception holds its thread, the thread its process, and the process the slot whose channel holds the message. A supervisor that never reads keeps a dead process alive through that cycle. Zircon's `ProcessDispatcher::FinishDeadTransition()` shuts the exceptionates down before anything else, and so does this: dropping the kernel end empties the cycle and tells the reader PeerClosed.

**Change**
- **ExceptionDispatcher** (`ObjectType::Exception`, default rights `Transfer | GetProperty | SetProperty | Inspect`, Zircon's). `on_zero_handles()` signals a response event: the handle's count reaching zero, however it got there, is the handler's answer. `wait_for_handle_close()` waits on that event interruptibly with Suspend masked, loops on `InternalIrqRetry` as Zircon's `WaitForHandleClose()` does, answers `Killed` on a kill, and otherwise reads and resets the state (`Handled` or `TryNext`). `discard_handle_close()` takes back the signal of a handle that died before reaching a handler. The event is this tree's manual-reset `sync::Event`, unsignalled by the waiter and by the discard. Zircon uses an `AutounsignalEvent`; with at most one handle out at a time the two behave the same.
- **Exceptionate**, with its own mutex. `set_channel()` answers InvArgs, BadState after shutdown, or AlreadyBound while the bound channel's reader is there, and replaces one whose reader has gone. `shutdown()` drops the channel and refuses later binds. `send()` builds `ExceptionInfo` (with `static_assert(user_copy::CopyAllowed<ExceptionInfo>)`: no byte the kernel did not write), mints the exception's handle and then the packet, because `MessagePacket::create()` takes finished handles, writes it, and on failure destroys any handle still on hand before `discard_handle_close()`, so that no late close re-signals. `PeerClosed` stands for Zircon's `ZX_ERR_NEXT`: no channel, or its reader has gone (then the dead end is dropped).
- **`Handle::make_uncharged()`**: a handle charged to no process, the user's decision of 2026-10-04 for the exception handle. `Handle::make()` charges the current process, which on the exception path is the faulting one, so a process at `HANDLE_MAX_PER_PROCESS` could not have been told about its own fault.
- **ProcessDispatcher** holds the Exceptionate, and `finish_dead_transition()` shuts it down first, outside the process lock (hard rule 1). `~Exceptionate()` shuts down too, for a task destroyed without dying.

Two departures in shape from Zircon. `send()` takes the `Arc` by value, because `ustl::Rc` propagates constness and a const reference would give only const access to the exception it may have to reset. And the task rights, `exception_get_thread` / `exception_get_process`, the report, and second chance are left out: slice 1 has no consumer for them.

**Reference**
Zircon `ExceptionDispatcher` (zircon/kernel/object/exception_dispatcher.cc: `on_zero_handles()`, `WaitForHandleClose()`, `DiscardHandleClose()`), `Exceptionate` (zircon/kernel/object/exceptionate.cc: `SetChannel()`, `Shutdown()`, `SendException()`), and `ProcessDispatcher::FinishDeadTransition()` (zircon/kernel/object/process_dispatcher.cc:547-554).

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (22 passed), rel PASS (20 passed), 0 error lines. Every process that dies in the boot (the 16 killed fault probes among them) now goes through `Exceptionate::shutdown()`.
- `ExceptionDispatcher` and `Exceptionate::send()` / `set_channel()` are compiled, not run: steps 4 and 5 bring their callers.
- clang-tidy: no finding in the new files or on the changed lines.

**Deliberately not done**
- The thread and job slots, the task rights and `exception_get_thread` / `_process`: not in slice 1.
- Adding the Exceptionate lock to `userspace-interface.md` 4.8's total order: with step 5, which first takes it under the ThreadDispatcher lock.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which references form the cycle that the shutdown in `finish_dead_transition()` breaks, and what keeps it alive without the shutdown?
- Why must a handle that `MessagePacket::create()` refused be destroyed before `discard_handle_close()`, not after?
- Why may `wait_for_handle_close()` unsignal the event after waking without racing a second signal?
