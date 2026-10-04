# kernel-panic outstanding repairs

## 1. A CPU that does not answer the suspend IPI holds a panic up for seconds under QEMU

**Severity**
Low: the report still comes out, only late, and only when a CPU is wedged with interrupts off.

**Symptom**
When some CPU cannot take the suspend IPI, `IpiService::suspend_others()` waits out its whole bound before the panic prints anything. The bound is 10 M spins of `arch::spin_hint()` (`kernel/kernel/task/ipi_service.cpp`). Under QEMU's TCG one spin of a similar loop measured about 640 ns (`docs/changes/261003/kernel-panic.md`, the panic-mode entry), so the wait can approach 6 s. On dbg, `crash panic-locked` leaves three CPUs spinning on the serial lock with interrupts off, and the banner came about 1.2 s after their last line.

**Cause**
The bound is a spin count, and a spin's cost varies by more than a hundred times. PAUSE takes about 10 cycles on some cores and about 140 on others, and under TCG it is far slower still. The serial logger's panic-mode wait had the same problem and was moved to TSC ticks for that reason. A CPU that loses `claim_report()` now clears its own Online bit, so this wait no longer happens when two CPUs panic together; it remains for a CPU that is truly wedged.

**Change**
Bound the wait in TSC ticks, as `PANIC_LOCK_TICKS` does in `kernel/lib/logz4/kernel.cpp`, sized to how long a healthy CPU needs to take an IPI. This is a latency bound on the panic path, so it is worth putting to the user with the measurement first: time the wait on dbg with `crash panic-locked`, before and after.

**Reference**
Linux `smp_send_stop()` (`arch/x86/kernel/smp.c`, `native_stop_other_cpus()`) waits up to one second, measured with `udelay()`, then sends an NMI and waits 10 ms more.

**Acceptance**
`crash panic-locked` on dbg prints its banner within the new bound after the command, and `ours-verify` passes.

## 2. Nothing can reset the machine without a panic, so the crash record's clear is untested end to end

**Severity**
Low: a missing test. The clear is covered by the host test of `crashlog::clear()`.

**Symptom**
`kshell_run.py --reboot "crash panic"` shows the second boot reporting the record, but no test shows that a third boot reports nothing. That would need a reset that is not a panic, and there is no reboot command.

**Cause**
`platform_reboot()` (`kernel/platform/pc/power.cpp`) is reachable only from `vpanic()`.

**Change**
A `reboot` kshell command in `kernel/commands`, after Zircon's `reboot` console command (`cmd_reboot()`, registered in `zircon/kernel/platform/power.cc`). Then `kshell_run.py --reboot` can run `crash panic` and check that a third boot, reached through `reboot` after the record was reported, logs `crashlog: no record from the previous boot`.

**Reference**
Zircon's `reboot` and `poweroff` console commands; Linux's `reboot(2)` with `LINUX_REBOOT_CMD_RESTART`.

**Acceptance**
A test boots, panics, reboots, sees the record, reboots through the new command, and sees no record.

## 3. The panic tests can no longer run on the rel image

**Severity**
Low: test coverage. The panic path is the same code on both images.

**Symptom**
`kshell_run.py --image rel "crash panic"` fails with no banner. The rel console answers `shell: launch failed: no such program in the BootFS: bin/crash`.

**Cause**
Since `ead5d12f` (another session's), the rel image runs the interactive user shell on the console, with the kernel shell off (`(kshell) not started (kernel.shell is off)`). `crash` is a kernel shell command, so nothing on rel can run it. Items 1 to 4 of `docs/changes/261003/kernel-panic.md` were verified on both images before that commit; everything after was verified on dbg only.

**Change**
Either a way to reach kernel commands from the user shell (the user shell already lists them, through `syscall::command_list`), or accept dbg-only panic tests and say so in the ours-verify skill. This is a decision for the user, because it is about what the user shell is allowed to do.

**Reference**
Zircon reaches its kernel console commands from user space through `zx_debug_send_command()`, which needs the debug resource.

**Acceptance**
`crash panic` runs on both images again, or the ours-verify skill states that panic tests run on dbg only.
