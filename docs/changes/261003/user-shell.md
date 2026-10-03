# user-shell changes

## Title: Replace the syscall demo with an interactive user shell (2026-10-03)

**Files**
- `system/public/ours/syscalls.gni` — add the generated, blocking `debug_read` ABI.
- `system/public/ours/syscall.hpp` — publish the bounded read size.
- `system/public/ours/syscall/resource.hpp` — reserve Zircon's System Debug resource slot.
- `kernel/lib/syscall/src/debug.cpp` — gate reads and validate the caller's resource.
- `kernel/platform/include/ours/platform/debug_console.hpp` — define the platform input boundary.
- `kernel/platform/pc/debug_console.cpp` — read COM1, waiting for the first byte and draining available bytes.
- `kernel/platform/arm/debug_console.cpp` and `kernel/platform/riscv/debug_console.cpp` — return `NotSupported` where serial input is not implemented.
- `kernel/include/ours/boot/options.hpp` and `kernel/top/boot_options.cpp` — add the opt-in `kernel.debug_read` flag.
- `kernel/lib/userabi/src/userboot.cpp` and `kernel/lib/userabi/userboot/userboot.cpp` — mint and forward the Debug resource only when enabled.
- `system/service/init/init.cpp` — duplicate launcher handles and pass them and the Debug resource to the shell.
- `examples/shell/shell.cpp` — add line editing, built-ins, and foreground BootFS program launch while keeping startup checks.
- `kernel/products/image/dev/BUILD.gn` — enable user debug input and disable the competing kernel shell.
- `.agents/tasks/user-shell/DESIGN.md` and `.agents/tasks/user-shell/PLAN.md` — record the accepted design and implementation.

**Symptom**
The program named `shell` only ran syscall smoke checks, printed the kernel command registry, emitted `--- end ---`, and returned. Userspace had output through `debug_write` but no input path, so it could not accept commands.

**Cause**
The shell target had grown as a ring-3 syscall test harness, but no syscall exposed serial input. `debug_write` is deliberately output-only. The only COM1 reader was the kernel shell, which runs in ring 0 and must not poll the same UART input concurrently with a user process. Simply adding a command parser would therefore still leave the shell unable to receive input, while reusing the kernel poller would violate the privilege boundary and let two consumers steal bytes from each other.

The shell also needs process capabilities and executable file bytes to run BootFS programs. Init originally gave it only a liveness channel and the root VMA. A fixed 64 KiB scratch buffer was insufficient even for the packaged shell executable, so it could not provide a general command-launch path.

**Change**
Before, the shell finished its test-only main after printing the boot marker:

```cpp
auto _ = ort::debug_line("--- end ---");
return 0;
```

After, the generated `debug_read` call blocks for serial input, and the user-mode command loop edits a bounded line, dispatches built-ins, or launches the named BootFS program through `launch::start()`:

```cpp
if (auto const status = ours::debug_read(debug_resource, input, sizeof(input), &actual);
    status != Status::Ok) {
    report("shell: debug_read = ", status);
    return 1;
}
```

The kernel requires both `kernel.debug_read=true` and a System Debug resource. Userboot creates and forwards that resource; init transfers it to the shell along with duplicated Job, BootFS, VMEX, and vDSO handles. The dev image sets `kernel.shell=false`, leaving one COM1 input owner. The shell maps the BootFS read-only once for its lifetime and uses spans into it for executable bytes, avoiding both the 64 KiB cap and a per-command mapping leak in the absence of `vma_unmap`.

The existing syscall checks still run before the prompt, preserving their output and the `--- end ---` marker expected by boot verification. Built-ins are `help`, `ls`, `echo`, `commands`, `clear`, and `exit`; other command names are resolved in BootFS and run in the foreground.

**Reference**
Zircon `sys_debug_read()` in `zircon/kernel/lib/syscalls/debug.rs` validates a System Debug resource and waits for the first character; `platform_dgetc()` in `zircon/kernel/platform/debug.cc` supplies the platform input. This implementation follows that debug-only contract, with PC polling in place of Zircon's UART receive queue. The launcher reuses this repository's `launch::start()` contract and VMA mapping API; no general terminal or file-descriptor service is introduced.

**Verification**
- `gn gen target/default` and `gn check target/default` passed.
- The changed shell and init translation units passed clang-tidy; the shell reports one pre-existing identifier-style warning for `H` in the old syscall checks.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` passed: debug image 19 passed, 0 skipped, 0 failed; release image 18 passed, 0 skipped, 0 failed.
- A live release QEMU session accepted `help` and `echo hello world`; `ls` listed the five BootFS entries; `shell` launched the packaged shell as a foreground child, completed its available startup checks, returned to the parent prompt, and `exit` closed the parent shell.
- Changed C++ and GN formatting checks passed.

**Deliberately not done**
This is bring-up debug I/O, not general stdin, VFS terminal support, pipes, quoting, history, job control, or an IRQ-driven UART receive queue. The PC read waits with a preemptible `pause` poll and can consume CPU while idle. ARM and RISC-V report `NotSupported`. The kernel shell is disabled in the dev image so it cannot compete for COM1 input.

**How it was found**
The original `examples/shell/shell.cpp` main printed `--- end ---` and returned. The syscall list and `ort::debug.hpp` showed output-only debug I/O; kernel `kshell` directly polled COM1. A live QEMU command exposed the fixed executable-size limit; mapping the BootFS once removed that limit without needing an unmap syscall.

**The user's prediction**
The user approved the debug-only serial input and exclusive user-shell ownership with “do it”; no separate prediction was stated. The observed competing-reader risk is addressed by setting `kernel.shell=false` in the user-shell image.

**Three questions you should be able to answer**
1. Why does `debug_read` require both a boot option and a System Debug resource?
2. Why is the BootFS mapped once for the shell's lifetime instead of mapped and unmapped for every command?
3. What would happen to serial input if `kernel.shell` and the user shell both read COM1?
