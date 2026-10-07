# object changes

## object_get_info answers HandleBasic: a handle's koid, rights, type and related koid (2026-10-07)

**Files**
- `system/public/ours/object_info.hpp` — new `InfoHandleBasic` record, 32 bytes, layout pinned by static_asserts; the topic comment says HandleBasic is answered.
- `system/public/ours/syscalls.gni` — `object_get_info`'s documentation: HandleBasic takes a handle of any type and asks for no right.
- `kernel/object/include/ours/object/dispatcher.hpp` — `Dispatcher::related_koid()`, virtual, 0 by default; `PeeredDispatcher` answers `m_peer_koid`.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — a process answers its job's koid.
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp` — a thread answers its process's koid.
- `kernel/object/include/ours/object/handle_table.hpp`, `kernel/object/handle_table.cpp` — `HandleTable::get_untyped()`: the object and the handle's rights, with no type or right required.
- `kernel/lib/syscall/src/object.cpp` — the HandleBasic case.
- `examples/shell/shell_tests.cpp` — new `check_handle_basic()` on both images; the probe check that expected NotSupported for HandleBasic now asks for topic 4, which nothing names; `check_exception_tasks()`'s comment no longer says the koids cannot be had.

**Symptom**
A program could hold a handle and still not know which object it named. `InfoTopic::HandleBasic` was numbered in the ABI but answered NotSupported. The profile program, holding `ort::process_self()`, still could not read its own koid to match against the `pid` in its samples.

**Cause**
Not a mistake so much as a step not taken yet: K24 answered only the Process topic. HandleBasic was left named and unanswered on purpose, as the example of "a topic the ABI numbers that this kernel does not answer".

What was missing is worth spelling out, because it shows what a handle is. The koid is a property of the object. The rights are a property of the handle: two handles to one process can carry different rights. Only the kernel sees both, since the handle table is kernel memory and user space holds nothing but the table index. So no user-space library could have worked either one out.

**Change**
The syscall looks the handle up without naming a type and without requiring a right, then builds the record outside the table lock:

```cpp
case InfoTopic::HandleBasic: {
    ustl::Arc<object::Dispatcher> dispatcher;
    auto rights = Rights::None;
    if (auto const status = ...handle_table().get_untyped(handle, &dispatcher, &rights); ...)
    auto const record = InfoHandleBasic{
        .koid = dispatcher->koid(),
        .rights = rights,
        .type = dispatcher->type(),
        .related_koid = dispatcher->related_koid(),
        .padding1 = {},
    };
    return single_record_result(buffer, buffer_size, actual, avail, record);
}
```

There is no right check, unlike Process, which needs Inspect. The record describes the caller's own handle. A holder can already learn the rights by trying calls, and the type by watching which ones answer BadType. Hiding the record would cost callers a probe and protect nothing.

`related_koid` is answered for the three objects bound to another from birth, as the user chose (option A):
- a process answers `m_job->koid()`;
- a thread answers `m_process->koid()`;
- a channel end answers `m_peer_koid`.

Every other object answers 0.

None of the three takes a lock, and the reason differs from object to object:
- `m_job` and `m_process` are set by the constructor and never reassigned, and the object they point to lives as long as they do.
- The channel is the interesting one. `m_peer`, the Arc to the other end, does change: `on_zero_handles()` empties it under the pair's shared lock when either end closes. So reading `m_peer->koid()` would need that lock, and could find `m_peer` empty.
- `m_peer_koid` is a plain number copied in `init_peer()`, before either end is reachable, and never written again. So it can be read with no lock, and it still answers after the peer has gone. The shell checks exactly that: it closes one end and reads the other's related koid again.

The worst interleaving the lock-free read has to survive is a close on another CPU in the middle of the get_info. With `m_peer_koid` that is harmless, because nothing writes it.

Rejected: option B, the field present but always 0 for now. It saves about twenty lines, but it would leave an ABI field whose 0 means either "no related object" or "not implemented yet", and a caller cannot tell which.

**Reference**
- Zircon `sys_object_get_info()`, case `ZX_INFO_HANDLE_BASIC` (`zircon/kernel/lib/syscalls/object.cc`): it requires no rights and fills `zx_info_handle_basic_t` from `handle->rights()`, `dispatcher->get_koid()`, `dispatcher->get_type()` and `dispatcher->get_related_koid()`. `InfoHandleBasic` keeps that layout: koid, rights, type, related_koid, then 8 bytes where Zircon has a reserved word and padding.
- Zircon `Dispatcher::get_related_koid()` and its overrides: `ProcessDispatcher` (the parent job), `ThreadDispatcher` (the process), and `PeeredDispatcher::get_related_koid()`, which returns `peer_koid_`, stored once by `InitPeer()`, for the same lock-free reason.

**Verification**
- Built and booted from `.worktrees/wip-proc-self` at 47940176 plus this change (the shared tree still holds another session's uncommitted VmoChildOptions change). `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`.
- `shell: handle basic checks passed` on both images. That covers:
  - a process related to its job;
  - a thread related to its process;
  - each channel end related to the other, kept after the peer closed;
  - a root region and a job with related koid 0;
  - a handle without Inspect answered with its own rights;
  - BadHandle for an invalid handle.
- The same check confirms the ProcSelf rights the previous change could not: ProcSelf has the same rights as a handle fresh from `process_create`, on dbg_1 (handed over by userboot) and on dev (by the root's runner).
- `shell: object_get_info checks passed` on dbg, with topic 4 now the one answered NotSupported.

**Deliberately not done**
- A job's related koid, its parent job: the user's option listed three objects. A job answers 0 until something needs its parent.
- Comparing the exception tasks' koids with `ExceptionInfo`'s in `check_exception_tasks()`: now possible, but it belongs to slice 2, step 1, of `.agents/tasks/exception-channels/`.
- The profile program's MODULE line, which is what this was for: the next sub-step.

**How it was found**
None.

**The user's prediction**
None. The user chose option A (related koid filled for process, thread and channel now) over B (the field zero for now).

**Three questions you should be able to answer**
- Why does HandleBasic need no right when Process needs Inspect, and why could only the kernel have answered it at all?
- A channel end's related koid is read while another CPU may be closing the peer. Why is reading `m_peer_koid` safe without the pair's lock, when reading through `m_peer` would not be?
- After one end of a channel closes, what does the other end's HandleBasic report as related_koid, and why is that the useful answer?
