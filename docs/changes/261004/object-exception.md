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

## Title: A user exception is sent to the process's exception channel, and the thread waits for the handler's answer (2026-10-04)

**Files**
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — `on_user_exception()` delivers through `deliver_exception()`, and reports and kills only what no handler resumed.
- `examples/shell/shell_tests.cpp` — each probe's exceptions are read and passed on before its answer is waited for; the verdict counts one exception per faulting probe and none for the control.
- `docs/design/260923/userspace-interface.md` — 4.8's total order, amended.
- `.agents/tasks/exception-channels/PLAN.md` — step 5 done.

**Symptom**
Before: a bound channel never saw an exception; every faulting probe died unannounced. After: the shell receives each fault probe's exception and passes it on, and the probe is killed as before. Step 5 of `.agents/tasks/exception-channels/`.

**Cause**
The delivery path (step 1), the objects (step 3) and the calls (step 4) existed, but nothing connected a fault to a channel. The connection is where every concurrent party meets: the faulting thread, a supervisor answering, and a kill that may come at any moment. So its shape follows from one rule, Zircon's: a kill always wins.

Three moments can see a kill. Before the send: the thread is Dying, and nothing is sent. During the wait: the interruptible wait answers `InternalIrqKilled`. And between the handler's close and the thread running again, which is the one that is easy to miss. Since `f94dd32b`, a kill that finds the waiter already claimed by the close's signal leaves the wake to it, so the wait answers Ok as if no kill had come. Only the thread's state tells the truth then, so the state is read again, under the lock, after the wait.

**Change**

```cpp
auto ThreadDispatcher::deliver_exception(ExceptionType const type) -> Delivery {
    auto self = ustl::Arc<Self>::try_adopt(this);
    auto exception = ExceptionDispatcher::create(ustl::move(self), type);
    ...
    {
        Guard guard{get_lock()};                 // the user's (a): send under this lock
        if (m_state != State::Running) {
            return Delivery::Killed;
        }
        if (m_process->exceptionate().send(exception) != Status::Ok) {
            return Delivery::Undelivered;
        }
    }
    auto const response = exception->wait_for_handle_close();   // no lock held
    Guard guard{get_lock()};
    if (m_state != State::Running || response == ExceptionDispatcher::Response::Killed) {
        return Delivery::Killed;                 // a kill wins over any answer
    }
    ...
}
```

`on_user_exception()` maps Handled to Resume, and Killed to End (the killer has sent the Kill signal). Undelivered and PassedOn kill the process outside the thread's lock, because the kill takes the process's lock and then each thread's. The fault report and register dump print only for Undelivered, as Zircon prints its crash report only for an exception no handler processed. A passed-on one gets a debug line. `try_adopt(this)` gives the exception its reference to the thread, since `adopt()` is only for an object nobody owns yet.

The shell has to change with it, or it deadlocks: a faulting probe now waits in the kernel for its answer, so the shell drains the exception channel (read, close: TryNext) until it closes with the probe's death, and only then reads the answer channel. There is no `object_wait_many` to wait on both.

The lock order gains ThreadDispatcher and Exceptionate after Process (4.8 amended), with the rule that no code takes a ThreadDispatcher lock while it holds a PeerHolder.

**Reference**
Zircon `ThreadDispatcher::HandleException()` (zircon/kernel/object/thread_dispatcher.cc:663-718), including the check after `WaitForHandleClose()` at :699-712, and `dispatch_user_exception()` (zircon/kernel/object/exception.cc), whose report prints only when `!processed`.

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (23 passed), rel PASS (20 passed), 0 error lines.
- dbg_1's log: all 16 fault probes `killed (retcode -1028)`; `exception channels delivered one exception to 16 of the probes, none to the control`; `exception channel checks passed`; no `terminating process` warning, since the shell saw every one.
- `waitqueue.test`, `sleep.test`, `threadjoin.test`: ok.
- clang-tidy: no finding on the changed lines.
- Not exercised: a kill during the wait. Nothing can kill another process's thread before `task_kill`. The wait's kill path is the one `waitqueue.test` checks for any interruptible wait.

**Deliberately not done**
- Checking the exception's type per probe, and resuming one: step 6.
- The thread and job slots, a debugger first: not in slice 1.

**How it was found**
None: a planned step.

**The user's prediction**
None. The lock order (send under the thread's lock) was the user's call, without the written argument.

**Three questions you should be able to answer**
- At which three moments can a kill reach an exception being delivered, and how does each one end?
- Why can the wait answer Ok for a thread that was killed, and what catches it?
- Why must the shell read the exception channel before the answer channel, and what happens if it does not?

## Title: exception_get_thread and exception_get_process hand a handler the faulting tasks, with the rights its channel was bound with (2026-10-04)

**Files**
- `kernel/object/include/ours/object/exception_dispatcher.hpp`, `kernel/object/exception_dispatcher.cpp` — the task rights (`set_task_rights()`), `make_thread_handle()`, `make_process_handle()`.
- `kernel/object/include/ours/object/exceptionate.hpp`, `kernel/object/exceptionate.cpp` — `set_channel()` takes the rights to hand out; `send()` gives them to each exception before its handle leaves the kernel.
- `kernel/object/include/ours/object/thread_dispatcher.hpp` — `process_ref()`, an `Arc` of the caller's own to the process.
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/exception.cpp` — the two calls; `task_create_exception_channel` passes its task handle's rights.
- `examples/shell/shell_tests.cpp` — every exception's thread and process are asked for and checked.
- `.agents/tasks/exception-channels/PLAN.md` — slice 2 planned, its step 1 done.

**Symptom**
None. A handler could see an exception and answer it, but could not reach the thread that raised it, and a handler that emulates a system call or fixes a fault has to read and write that thread's registers. Step 1 of slice 2, the thread-level capture the user asked for as the base of a Starnix-like supervisor.

**Cause**
The handle a handler needs is a capability, so the question is which rights it carries. Zircon's answer: the rights of the task handle the channel was bound with. Whoever bound the channel could have reached the task through that handle anyway, so the channel grants nothing it did not already have. The rights are recorded on the exceptionate at bind time and copied onto each exception as it is sent, before its handle reaches user space; one handler holds an exception at a time, so they never change under one. A channel bound to a thread will withhold the process (rights None): it grants no way up the task chain.

**Change**
- `Exceptionate::set_channel(channel, thread_rights, process_rights)`; `send()` calls `exception->set_task_rights()` before minting the exception's handle.
- `ExceptionDispatcher::make_thread_handle()` / `make_process_handle()`: AccessDenied for rights None; otherwise a new handle charged to the calling process (the handler's to keep or close), NoResource when refused. The `Arc<Dispatcher>` is spelled out at `Handle::make()`, because an `Arc<ThreadDispatcher>` converts to both its first parameters, `KernelHandle` and `Arc`.
- `exception_get_thread` / `exception_get_process`: the exception handle needs no right, as in Zircon; BadType for anything else; the value goes out before the handle is installed (design 4.1).
- `task_create_exception_channel` binds with its handle's rights for both thread and process, Zircon's choice, including Zircon's acknowledged imprecision that task rights do not map one to one.
- The shell asks each exception for its process (which must answer `object_get_info(Process)`) and its thread (a live handle that must not), and once checks that a job handle is BadType. The koids cannot be compared with `ExceptionInfo`'s until `object_get_info` answers HandleBasic.

**Reference**
Zircon `sys_exception_get_thread()` / `sys_exception_get_process()` and the rights in `sys_task_create_exception_channel()` (zircon/kernel/lib/syscalls/exceptions.cc), `ExceptionDispatcher::SetTaskRights()`, `MakeThreadHandle()`, `MakeProcessHandle()` (zircon/kernel/object/exception_dispatcher.cc), `Exceptionate::SendException()`.

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (24 passed), rel PASS (21 passed), 0 error lines.
- dbg_1's log: `exception_get_thread(job) = BadType`; no exception_get or object_get_info failure for any of the 16 probes; `exception channel checks passed`.
- clang-tidy: no finding on the changed lines.

**Deliberately not done**
- Comparing koids: needs `object_get_info(HandleBasic)`.
- The thread slot itself: step 2 of slice 2.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a handle got from an exception carry the rights of the task handle the channel was bound with, and what would go wrong if it carried the task's default rights?
- Why must `send()` set the task rights before the exception's handle is minted, rather than after the write?
- Why does a thread-level channel hand out no process handle?
