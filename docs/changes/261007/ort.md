# ort changes

## Every starter hands a new process its own handle, and the runtime keeps it as ort::process_self() (2026-10-07)

**Files**
- `system/lib/ort/src/start.cpp` — takes the ProcSelf startup handle before main and keeps it; `process_self()` returns it. The comment on `ort_extensions_init`, which a formatter had folded into the middle of its declaration, is one comment again.
- `system/lib/ort/include/ort/startup.hpp` — declares `ort::process_self()`: borrowed, never closed by the caller.
- `kernel/lib/userabi/src/userboot.cpp` — `launch()` also hands back a handle to the process, with the rights `ProcessDispatcher::create()` gave; `write_bootstrap()` puts it in userboot's processargs as ProcSelf.
- `kernel/lib/userabi/userboot/userboot.cpp` — the next program gets the process handle userboot used to close, as its ProcSelf; userboot checks its own.
- `system/lib/launch/include/launch/process_launcher.hpp` — `LAUNCHER_HANDLE_INFO` gains ProcSelf, so `MAX_HANDLES` goes from 62 to 61.
- `system/service/init/launcher_server.cpp` — the Launcher duplicates the started process with SameRights and writes it third, after VmaRoot and VmoVdso.
- `system/service/init/realm.cpp` — the ELF runner lists ProcSelf beside VmaRoot and adds a duplicate of the process it keeps to supervise.
- `system/service/init/init.cpp` — autorun hands the program the process handle it used to close.
- `examples/shell/shell.cpp`, `examples/shell/shell_tests.cpp` — dbg_1's library starts and the fault probes get a duplicate; a new `check_process_self()`; `TOO_MANY_HANDLES` follows the Launcher's three.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the component and its spawned copy check their ProcSelf; `TOO_MANY_HANDLES` follows the Launcher's three.
- `system/lib/launch/test/process_launcher_test.cpp` — the counts follow the third launcher handle; the parse helper reads `LAUNCHER_HANDLE_INFO.size()` instead of a literal 2.
- `docs/design/261004/component-framework.md` — D19 amended, and the counts that follow from it.

**Symptom**
A program could not name its own process. The profile program needs its process's koid to print a MODULE line that ties its user samples (each carries `pid`, the koid) to its binary, and there was no handle to ask `object_get_info` about.

**Cause**
The processargs protocol has had a ProcSelf type (0x01) from the start, but no starter wrote one. Each had its own reason:
- The kernel and userboot built their message lists before the process existed and simply listed what the next program needs to start others, not what it needs to know about itself.
- The Launcher's design (D19) took Fuchsia's set "less ProcessSelf, ThreadSelf and StackVmo", because `launch::start()` closes the thread and the stack VMO, and nobody had a use for a self handle at the time.
- Autorun and userboot even held a process handle they had no use for, and closed it.

A process cannot make this handle for itself later: every system call names its object by a handle the caller already holds, so a process with no handle to itself has no way to get one. It has to come from whoever created the process, at the one moment that party holds both the process handle and the bootstrap channel.

**Change**
One rule for every starter: the processargs carries ProcSelf, a handle to the new process with every right the creator's handle had.

Where the starter keeps its own handle (the Launcher, whose client gets the process in the reply; the realm, which supervises; the shell, which waits), it writes a duplicate:

```cpp
// launcher_server.cpp, after launch::start()
auto self = Uandle::Invalid;
if (auto const status =
        ours::handle_duplicate(started->process, ours::Rights::SameRights, &self);
    status != Status::Ok) { ... }
handles[count] = started->root_vma;
handles[count + 1] = vdso;
handles[count + 2] = self;
```

Where it used to close its handle (userboot, autorun), it hands that handle over instead:

```cpp
// before (userboot start_next)
auto _ = ours::handle_close(started.unwrap().process);
// after
handles[1] = started.unwrap().process;
```

The kernel makes a second user handle from the same dispatcher. `Handle::make()` raises the dispatcher's handle count. The `KernelHandle<ProcessDispatcher>` that `launch()` drops at its end calls `on_zero_handles()`, which does nothing to a process that has left `State::Initial`, so the running process is untouched.

The runtime takes ProcSelf in `read_processargs()`, before `ort_extensions_init()` and main, and keeps it. `take_startup_handle(ProcSelf, 0)` therefore finds nothing, and every caller of `process_self()` borrows the same handle. Taking it in the runtime rather than leaving it in the table means no library has to agree on who owns it, and nobody can close it by taking it.

Rejected:
- A reduced-rights ProcSelf (e.g. no Destroy). The user chose full rights, which is what process_builder gives. A program can kill itself with `process_exit` anyway, so withholding the right protects little.
- A new system call that answers the caller's own koid. That would be a second way to name the current process, beside handles, for one fact; with a handle, `object_get_info` answers this and everything later.

**Reference**
- Fuchsia process_builder `ProcessBuilder::new()` adds `HandleType::ProcessSelf` (a duplicate of the process) to the processargs, and `ProcessBuilder::build()` writes it with the rest (`src/lib/process_builder/src/process_builder.rs`).
- Fuchsia libc `__libc_start_main()`: while reading the processargs it takes PA_PROC_SELF into `__zircon_process_self`, which `zx_process_self()` returns, borrowed, for the life of the process. `ort::process_self()` is the same arrangement.

**Verification**
- Built and booted from a clean worktree at HEAD 200a0370 plus this change (`.worktrees/wip-proc-self`), since the shared tree holds another session's uncommitted VmoChildOptions change. `boot_check.py`: `dbg PASS 34 passed, 3 skipped, 0 failed`; `rel PASS 32 passed, 9 skipped, 0 failed, 6/6 required`.
- `shell: process self checks passed` on both images. That covers userboot→shell on dbg_1 and the realm runner→shell on dev.
- `routing_checks: passed` on rel. It now includes the component's own check (realm runner) and its spawned copy's (Launcher).
- No `userboot: ... FAILED` line on either image, which covers kernel→userboot.
- Host tests: `process_launcher` 15/15, `fsio` 34/34, `autorun` 5/5, `process_args` 16/16.
- Not checked: that each ProcSelf carries full rights. Reading a handle's rights back needs InfoTopic::HandleBasic, which is the next step.

**Deliberately not done**
- ThreadSelf: `launch::start()` closes the thread, and nothing yet asks a thread to name itself.
- `object_get_info(InfoTopic::HandleBasic)`, the koid itself: the next sub-step, introduced on its own.
- The fault probe and autorun programs do not check their ProcSelf; the shell, routing_checks and userboot cover every starter between them except the probe path, which uses the same code as dbg_1's library start.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a process not obtain a handle to itself after it has started, and who therefore has to give it one?
- The kernel drops its `KernelHandle<ProcessDispatcher>` at the end of `launch()`, which calls `on_zero_handles()`. Why does that not kill userboot, now that userboot also holds a user handle to the same process?
- Why does the runtime take ProcSelf out of the startup handle table before main, instead of leaving it for whichever library wants it?
