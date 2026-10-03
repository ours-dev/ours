# syscall changes

## Title: object_get_info answers the Process topic: a parent reads how its child ended (2026-10-03)

**Files**
- `system/public/ours/object_info.hpp` (new) — `InfoTopic`, `InfoProcessFlags` and `InfoProcess`, with the record's layout pinned by `static_assert`.
- `system/public/public_headers.gni` — lists it.
- `system/public/ours/syscalls.gni` — the `object_get_info` entry, after `object_wait_one`.
- `system/public/ours/vdso.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — include the new header, which the generated declarations name.
- `kernel/lib/syscall/src/object.cpp` — `syscall::object_get_info()` and `single_record_result()`.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — `m_start_time`, written by `attach_thread()` with the first thread, and `get_info()`.
- `examples/shell/shell.cpp` — `run_probe()` hands back the process handle a launcher run gets; `check_object_get_info()` checks the call on the control probe.

**Symptom**
None: a new system call, step K24 of `docs/design/260924/userspace-filesystem.md`. Before it, a program that started another could wait for it to end (TERMINATED, K7) but not learn how. The shell judged a probe "killed" by whether its answer channel closed empty.

**Cause**
The objects:
- A process handle, which the launcher gives the program that started the process.
- `ProcessDispatcher::m_retcode`, the code the process ended with. `exit()` or `kill()` writes it under `get_lock()`, together with `m_state`. Nothing read it until now.
- A topic, which names one record layout. `object_get_info` is one call for every question about an object, so a new question is a new topic, not a new system call.
- `Inspect`, the right a handle needs to be asked about.

The events, for the shell and its control probe:
1. The shell starts `fault_probe none` with the launcher and keeps the process handle.
2. The probe returns 2 (`UNKNOWN`) from main. The process goes Dying, then Dead, and TERMINATED is raised last, after the address space and the handle table are gone.
3. The shell waits for TERMINATED, then calls `object_get_info(process, InfoTopic::Process, &info, 24, &actual, &avail)`.
4. The kernel looks at the topic, then resolves the handle as a process with `Inspect`, and copies one record out.

The record's three values must come from one critical section. `kill()` writes the code and the state under one lock hold. A reader taking them in two holds could see Exited paired with the code from before the kill. So `get_info()` copies the state, code and start time in one hold, and works out the flags after releasing the lock.

`actual` and `avail` count records, not bytes. A buffer too small for the record still gets `avail`, so the caller learns what to ask for.

**Change**
The record is Zircon's `zx_info_process_t` (version 1 of topic 3):

```cpp
struct InfoProcess {
    i64 return_code;          // valid once Exited
    Instant start_time;       // valid once Started
    InfoProcessFlags flags;   // Started = bit 0, Exited = bit 1
    u8 padding1[4];           // zero: no kernel stack leaks out
};
static_assert(sizeof(InfoProcess) == 24);
```

The snapshot, in `ProcessDispatcher::get_info()`:

```cpp
{
    Guard guard{get_lock()};
    state = m_state;
    info.return_code = m_retcode;
    info.start_time = m_start_time;
}
switch (state) {
    case State::Dead:
    case State::Dying:
        info.flags |= InfoProcessFlags::Exited;
        [[fallthrough]];
    case State::Running:
        info.flags |= InfoProcessFlags::Started;
        break;
    case State::Initial:
        break;
}
```

The handler checks in Zircon's order:
1. An unknown topic is NotSupported, before the handle is looked at.
2. For Process, the handle resolves as BadHandle, then BadType, then AccessDenied without `Inspect`.
3. The record is written, then `actual` and `avail`. A short buffer is BufferTooSmall with `actual` 0 and `avail` 1.

The buffer arrives as bytes. Once the topic fixes the type, it is retyped as `UserOutPtr<InfoProcess>`, as Zircon's `reinterpret` does.

The start time is `SteadyClock::now()` in nanoseconds, taken in `attach_thread()` under the lock and just before the state becomes Running. A reader that sees Running therefore sees the time.

Rejected: a 16-byte record with the code and the flags only. The user chose Zircon's layout, start time included, so that the ABI does not need a new version for it later.

**Reference**
- Zircon `sys_object_get_info()` (`kernel/lib/syscalls/object_info.rs`), with `single_record_result()` and `actual_avail_result()`; the ZX_INFO_PROCESS case is in `object_info_ffi.cc`.
- Zircon `ProcessDispatcher::GetInfo()`, the same snapshot-then-switch, and `ProcessDispatcher::AddInitializedThread()`, which sets `start_time_` with the state.
- `zx_info_process_t` and `ZX_INFO_PROCESS_FLAG_*` in `zircon/syscalls/object.h`.

**Verification**
- The shared tree, holding another session's uncommitted user-shell work as well, builds with `ninja -k 0`.
- Boot verification ran in a copy of HEAD with only this change on top. `boot_check.py`:
  - dbg PASS, 20 passed: one more than before, `object_get_info checks passed`;
  - rel PASS, 13 passed, 10 skipped: rel runs no probes, so it does not reach the check.
- The dbg checks, all as expected:
  - TERMINATED, then Ok, return code 2, flags Started and Exited, `actual` 1 and `avail` 1;
  - one byte short: BufferTooSmall, `actual` 0, `avail` 1;
  - HandleBasic: NotSupported;
  - the job handle: BadType;
  - a duplicate without `Inspect`: AccessDenied.
- 15 probes killed, launcher checks passed.
- Not exercised: a kill racing `get_info()`. The one-hold snapshot rests on the argument above.

**Deliberately not done**
- Only the control probe is checked. The faulting probes are started by `process_spawn_boot`, which returns no process handle; moving them to the launcher, and reading each kill's code, is U6.
- No `HandleBasic`, `Thread` or any other topic.
- No `DEBUGGER_ATTACHED` flag: there is no debugger.

**How it was found**
None: a new feature.

**The user's prediction**
None. The user chose Zircon's 24-byte layout over a 16-byte one.

**Three questions you should be able to answer**
- Why must `get_info()` copy the state and the return code in one hold of the lock, and what could a reader see otherwise?
- Why does the shell wait for TERMINATED before asking, when the probe has already answered on its channel?
- Why is `avail` written even when the buffer is too small, and why does it count records rather than bytes?
