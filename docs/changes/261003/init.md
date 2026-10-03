# init changes

## Title: init starts the shell with the launcher and reports the code it exits with (2026-10-03)

**Files**
- `system/service/init/init.cpp` — init now:
  - takes its own VmaRoot;
  - makes a `launch::BootfsLauncher`;
  - starts the shell through `start_then_write()`, which used to be `spawn_then_write()` and now takes an optional launcher and hands back the process handle;
  - after the shell lets go of the channel, waits for TERMINATED and prints `init: shell exited with <code>` (`report_exit()`);
  - prints the launcher's errors (`print_launch_error()`).
- `system/service/init/BUILD.gn` — depends on `//system/lib/launch`.
- `system/lib/ort/include/ort/debug.hpp`, `system/lib/ort/src/debug.cpp` — `signed_decimal()`, for a return code.
- `examples/shell/shell_tests.cpp` — uses `ort::signed_decimal()` instead of its own copy.

**Symptom**
None: the second half of step U7 in `docs/design/260924/userspace-filesystem.md`. Before it, the kernel spawned the shell (`process_spawn_boot`), so init had no process handle. It knew only that the shell had closed its channel, not that the shell had ended or how.

**Cause**
The objects:
- init's root region (VmaRoot), where the launcher maps the BootFS.
- The BootFS launcher (`launch::make_bootfs_launcher()`, moved into the library just before this).
- The shell's process handle, which only a user-space launch hands back.
- The liveness channel. The shell answers "pong" on it, then closes its end.

The events, on the rel image, where userboot starts init:
1. init makes the launcher.
2. It starts the shell with it, keeping the process handle, and writes the processargs.
3. The shell answers "pong" and closes the channel. init prints `init: shell closed its channel`.
4. init waits for TERMINATED on the shell's process. The shell is interactive, so this sleeps until someone types `exit`.
5. The shell returns 0. The process dies, and TERMINATED is raised last.
6. init reads the code with `object_get_info` and prints `init: shell exited with 0`.

A closed channel says only that the shell is done with init. The process may still be running, as it is here for as long as the prompt is up. TERMINATED on the process handle is the sign that it is gone.

**Change**

```cpp
// before
auto const spawned = ours::process_spawn_boot(job, name.data(), name.size(), there, &root_vma);

// after: the launcher when init has one, the kernel otherwise
if (launcher != nullptr) {
    auto const started = launch::launch_from_bootfs(*launcher, job, name, there, {...});
    ... *process = started.unwrap().process; root_vma = started.unwrap().root_vma; ...
} else {
    spawned = ours::process_spawn_boot(job, name.data(), name.size(), there, &root_vma);
}
```

The fallback stays. If the BootFS, vmex, the vDSO's VMO or VmaRoot is missing, or a launcher step fails, init prints `init: no launcher; the kernel spawns the shell` and goes on as before, without a handle to wait on. The shell is the system's console, and a boot without one is worse than a boot without the exit report.

**Reference**
- Fuchsia's console-launcher starts the console shell with fdio's spawn and keeps the process: `src/bringup/bin/console-launcher/console_launcher.cc` waits for `ZX_PROCESS_TERMINATED` on it, then reads `ZX_INFO_PROCESS`, the same two calls in the same order as `report_exit()`.

**Verification**
- `ninja -k 0` on `out/bringup.x86`: no error or warning.
- `boot_check.py`:
  - dbg PASS, 20 passed;
  - rel PASS, 18 passed. The rel log has `init: launched shell with the launcher`, `init: shell says "pong"` and `init: shell closed its channel`.
- On the rel image's serial port, typing `exit` at the user shell's prompt ends it, and init prints `init: shell exited with 0`.
- Not exercised: the fallback, when a launcher handle is missing. Every image hands init all four.

**Deliberately not done**
- The autorun program is still spawned by the kernel. U7 moves the shell only; the autorun path calls `start_then_write(nullptr, ...)`.
- The design names the program `/boot/shell`. The BootFS here has no directories, so it is `shell`.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a closed liveness channel not mean the shell has ended, and what does?
- Why does init keep a fallback to `process_spawn_boot` rather than halting when it cannot make a launcher?
- Where does the launcher map the BootFS, and why does init need a VmaRoot startup handle for that?

## Title: init starts the autorun command with the launcher too, and never asks the kernel to load a program (2026-10-03)

**Files**
- `system/service/init/init.cpp`:
  - `start_then_write()` takes a launcher by reference and no longer has a `process_spawn_boot` branch;
  - `start_autorun()` starts the command with the launcher and closes the process handle it gets;
  - `main()` halts with `init: cannot start programs (...)` when it cannot make a launcher, instead of falling back to the kernel.

**Symptom**
None. The user set the rule: once user space runs, it uses nothing on the kernel side except system calls. `process_spawn_boot` is a system call, but what it does is the kernel's own ELF loading from its package. init still used it for the autorun command, and as a fallback for the shell.

**Cause**
The objects:
- `process_spawn_boot`: the kernel finds a program by name in its resident package and loads it with `userabi::launch()`. That is loader policy running in ring 0.
- The user-space launcher (`launch::launch_from_bootfs()`): the same work done by the calling process, using only object system calls (`process_create`, `vma_map`, `vmo_create_child`, `thread_create`, `process_start`).

Each program that `spawn_boot` starts keeps a second loader alive in the kernel, one that user space could do without. Zircon's kernel loads exactly one program, userboot. Everything after it, userboot's own next program included, is loaded from user space.

The fallback hid a broken launcher. A boot whose launcher failed would still show a shell, started by the kernel, and the failure would read as one line among many.

**Change**

```cpp
// before
auto start_then_write(launch::BootfsLauncher const *launcher, ...) {
    if (launcher != nullptr) { ... launch_from_bootfs ... }
    else { spawned = ours::process_spawn_boot(job, name.data(), name.size(), there, &root_vma); }

// after: the launcher, or nothing
auto start_then_write(launch::BootfsLauncher const &launcher, ...) {
    auto const started = launch::launch_from_bootfs(launcher, job, name, there, {...});
```

init does not wait for the autorun program. It closes the process handle at once: closing a handle does not stop a process that has started. Only a process still Initial dies with its last handle.

**Reference**
- Zircon's userboot is the only program its kernel loads (`zircon/kernel/lib/userabi/userboot.cc`). console-launcher starts both the shell and `zircon.autorun.boot` itself.

**Verification**
- `boot_check.py` on `out/bringup.x86`: dbg PASS (20 passed), rel PASS (18 passed); the rel log has `init: launched shell`.
- `out/autorun-launch` is the same build with `ours = { with = [fault_probe], autorun = "fault_probe+none" }` (tools/set.py). On it:
  - dbg PASS, rel PASS;
  - the rel log has `init: autorun fault_probe+none`, then the probe's own `fault_probe: unknown probe none` and `fault_probe: no answer channel (User0)`, so the launcher started it and it reached main.
- `git grep spawn_boot system/service/init`: nothing.

**Deliberately not done**
- userboot still starts its next program with `process_spawn_boot` (step U8).
- The shell's self-checks still call it when they have no launcher.
- The system call itself stays until K25.

**How it was found**
None: the user's rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does `process_spawn_boot` do in the kernel that the user-space launcher does with ordinary object system calls?
- Why can init close the autorun program's process handle at once without killing it?
- What did the fallback to the kernel hide?
