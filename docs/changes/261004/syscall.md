# syscall changes

## Title: task_create_exception_channel and object_set_property, for a process's exception channel (2026-10-04)

**Files**
- `system/public/ours/syscalls.gni` — the two entries.
- `system/public/ours/vdso.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — include `<ours/exception.hpp>` and `<ours/object_property.hpp>`, whose types the calls take.
- `kernel/lib/syscall/src/exception.cpp` — new: `task_create_exception_channel()`.
- `kernel/lib/syscall/src/object.cpp` — `object_set_property()`.
- `kernel/lib/syscall/BUILD.gn` — the new file.
- `examples/shell/shell_tests.cpp` — every fault probe gets a channel, checked; the refusals once; a verdict line.
- `.agents/tasks/exception-channels/PLAN.md` — step 4 done.

**Symptom**
None: nothing is sent on a channel yet. Step 4 of `.agents/tasks/exception-channels/`.

**Cause**
The exceptionate (step 3) has no way in from user space. Zircon has two calls for it: one to bind a channel to a task, and the generic property call a handler answers an exception with.

**Change**
- `task_create_exception_channel(handle, options, &out)`. `options` must be None. The handle must name a process and grant Inspect, Duplicate, Transfer, ManageThread and Enumerate, Zircon's rights. A thread or a job answers NotSupported rather than BadType: Zircon binds to both, and they are missing here, not wrong. Then a channel pair is made. The reading end gets `Transfer | Wait | Read`, Zircon's `kExceptionChannelRights`, because the kernel's end never reads: anything a program could write to it would sit there as long as the process lives. The order follows design 4.1: the reading end's value is written to the program, then the kernel's end is bound (`Exceptionate::set_channel()`, which may still answer AlreadyBound or BadState), and only then is the reading end installed. A refusal at any point leaves nothing installed.
- `object_set_property(handle, property, value, size)`, Zircon's signature. The property is looked at first, as `object_get_info()` looks at the topic first: the table has no lookup that does not name the object's type. ExceptionState wants SetProperty on an exception handle (BadType for anything else) and at least 4 bytes (BufferTooSmall). The value is read as a plain `u32` and checked against TryNext and Handled (InvArgs otherwise) before it becomes an `ExceptionState`, since a value read straight from user memory could be any number. An unknown property is InvArgs, Zircon's answer, though here it comes before the handle check.
- The shell binds a channel to each of the 16 fault probes after the launcher returns the process and before the processargs go out (the probe waits for them, so it cannot fault first). It checks that a second bind is AlreadyBound, that `channel_write` on the reading end is AccessDenied, and that the channel reports PeerClosed, with nothing to read, once the probe is dead. Once: the root job answers NotSupported, and `object_set_property(ExceptionState)` on the job is BadType. `shell: exception channel checks passed` or FAILED carries the verdict to `boot_check.py`.

**Reference**
Zircon `sys_task_create_exception_channel()` (zircon/kernel/lib/syscalls/exceptions.cc) and `sys_object_set_property()`'s `ZX_PROP_EXCEPTION_STATE` case (zircon/kernel/lib/syscalls/object.cc).

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (23 passed: the exception channel checks are new), rel PASS (20 passed), 0 error lines.
- dbg_1's log: `task_create_exception_channel(job) = NotSupported`, `object_set_property(job, ExceptionState) = BadType`, `exception channels behaved on 16 of the probes`, `exception channel checks passed`.
- clang-tidy: no finding in the new file or on the changed lines.
- Not exercised: `object_set_property` on a real exception (step 5 creates the first one).

**Deliberately not done**
- Sending on the channel: step 5. Until it lands the probes still die unannounced, so a bound channel shows only PeerClosed.
- Threads and jobs, the debugger option, and `exception_get_thread` / `_process`: not in slice 1.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the reading end not be written, and what would a writable one let a program do?
- Why is the reading end's value written to the program before the channel is bound, and what does a program hold when the bind is then refused?
- Why is the value of `object_set_property` read as a `u32` and checked before it becomes an `ExceptionState`?
