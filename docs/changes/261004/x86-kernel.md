# x86-kernel changes

## Title: A user exception's delivery answers Resume or End, so a handled exception can return to ring 3 (2026-10-04)

**Files**
- `kernel/include/ours/task/exception.hpp` — `task::ExceptionOutcome { Resume, End }`.
- `kernel/arch/x86/kernel/faults.cpp` — `deliver_user_exception()` is no longer `[[noreturn]]`: it answers the outcome, and the exception tail runs the one pending-signal check and panics only if a thread told to End comes back from it.
- `kernel/include/ours/task/thread.hpp`, `kernel/kernel/task/thread.cpp` — `Thread::Current::on_user_exception()` passes the outcome up.
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp` — `ThreadDispatcher::on_user_exception()` answers it; `on_policy_exception()` drops it, as the system-call layer answers BadSyscall either way.
- `.agents/tasks/exception-channels/PLAN.md` — step 1 as built.

**Symptom**
None: behaviour is the same. Every user exception still kills its process, and every fault probe on dbg_1 still ends with `killed (retcode -1028)`. This is step 1 of `.agents/tasks/exception-channels/`.

**Cause**
The delivery path could not express a thread that survives its exception. `deliver_user_exception()` was `[[noreturn]]`: it called up to the object layer, which killed the process, ran the pending-signal check, which then exited the thread, and panicked if it ever got past that. An exception channel's handler has to be able to say "handled", and then the thread has to go back to ring 3 through the very frame the exception saved, possibly with registers the handler changed. The way back already existed: the interrupt stub reloads every register from that frame and executes `iretq`, and nothing stood in the way except a function that refused to return.

**Change**
The object layer decides, and only its answer crosses the layers:

```cpp
// before
[[noreturn]] auto deliver_user_exception(...) -> void {
    ...
    Thread::Current::on_user_exception(context);   // kills the process
    Thread::Current::process_pending_signals();    // exits the thread
    panic("a user exception ({}) did not end thread {}", ...);
}

// after
[[nodiscard]] auto deliver_user_exception(...) -> task::ExceptionOutcome {
    ...
    arch::AutoInterruptEnabler const irqs_on;
    return Thread::Current::on_user_exception(context);
}
...
auto const outcome = [&] {
    if (!raised) {
        return task::ExceptionOutcome::Resume;
    }
    return deliver_user_exception(frame, user_exception);
}();
Thread::Current::process_pending_signals();
if (task::ExceptionOutcome::End == outcome) {
    panic("a user exception ({}) did not end thread {}", ...);
}
```

Two values, not the three dispositions an exception channel has (handled, try next, thread exit): which thread or process dies is the object layer's business, and it carries it out by giving the thread the Kill signal, so the architecture only needs to know whether to expect to come back. The panic keeps the invariant the old body checked, now for End only. `on_bad_syscall()` stays `void`: `not_from_vdso()` writes `BadSyscall` into rax whatever the outcome, as Zircon's `sys_invalid_syscall()` returns `ZX_ERR_BAD_SYSCALL` after raising the policy exception, so `//kernel/lib/syscall` needs no task-layer type (design 4.7). The opaque `enum class ExceptionOutcome : u8;` declarations in `thread.hpp` and `thread_dispatcher.hpp` follow the one `PolicyCode` already has there.

**Reference**
Zircon `try_dispatch_user_exception()` (zircon/kernel/arch/x86/exceptions_c.cc), whose bool says whether a handler took the exception, ahead of the pending-signal check in `x86_exception_handler()`; `dispatch_user_exception()` (zircon/kernel/object/exception.cc) ends the thread itself otherwise.

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (22 passed), rel PASS (20 passed), 0 error lines.
- dbg_1's log: all 16 fault probes `killed (retcode -1028)`, as before.
- clang-tidy: no finding on the changed lines.
- Resume itself is not exercised: nothing answers it until step 5.

**Deliberately not done**
- Answering Resume: steps 3 to 5 of the plan.
- The deferred policy exception (Zircon's `THREAD_SIGNAL_POLICY_EXCEPTION`), which a handler that writes registers needs: slice 2.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which code returns a resumed thread to ring 3, and why does it need no change for a handler to edit the thread's registers?
- Why does the architecture need only Resume and End, when a handler can answer three ways?
- Why may `not_from_vdso()` ignore the outcome?
