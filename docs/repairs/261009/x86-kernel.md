# x86-kernel outstanding repairs

## 1. A CPU exception in kernel mode stops that one CPU, silently, instead of panicking

**Severity**
Medium: it produces no wrong result of its own, but every kernel fault is reported late and looks like something else. Any CPU exception taken in kernel mode reaches it: a #UD from a compiler trap, a #GP, a page fault that cannot be resolved.

**Symptom**
The faulting CPU logs the exception's name and its registers, then halts forever with interrupts off, still holding whatever spin locks it held. The other CPUs keep running. There is no `OURS KERNEL PANIC` banner, no backtrace, no crash record, and no reset under `kernel.panic.reboot`. Ten seconds later the lockup detector finds the CPU, reports `took no tick for 10 s` and panics, so the report reads as a lockup.

On 2026-10-09 a #UD in `WaitQueue::unpark_thread()` on cpu0 came out as `lockup: cpu0 took no tick for 10 s` with `cpu0 thread Idle/0 at +0x15721, interrupts off, 2 spin locks held` and a backtrace of `+0x15721` alone. `+0x15721` is the halt loop of `panic_exception()`, and the two locks were the ones `unpark_thread()` held when it trapped. The fault's own line, `#UD Invalid opcode`, came 10.8 s earlier, among thousands of debug lines (`out/hangs/2026-10-09-interruptline-thread-exit-edit-run10-dbg.log` in the project root). The #UD itself came from an uncommitted edit to `Thread::Current::exit()` that frees a joinable thread nobody has joined yet, so a later `join()` works on freed memory; committed code does not do that. A CPU that then needs one of those locks spins on it with interrupts off and stops ticking as well, which reads as a deadlock.

**Cause**
`panic_exception()` (`kernel/arch/x86/kernel/faults.cpp`) was written before the panic path existed. It puts the registers on the serial line and ends in its own `while (1) arch::halt()`, without calling `panic()`. So it skips everything `vpanic()` does: claiming the report, entering panic mode, stopping the other CPUs, replaying the record ring, writing the crash record and resetting. It also prints no backtrace from the faulting frame. The lockup detector's walk, 10 s later, starts inside the halt loop and does not get past the exception frame.

**Change**
End `panic_exception()` in `panic()`, with a message that names the exception and the offset of `rip`. Before that, print the registers as now and a frame-pointer backtrace that starts from the frame's `rip` and `rbp`, using `sampler::walk_frame_pointers()` as the lockup detector does rather than a second walker.

One ordering question for whoever takes this. `vpanic()` stops the other CPUs only once it is called, so register and backtrace lines printed before it can interleave with their output. Putting them after the stop needs a hook in `vpanic()` or a fault-specific entry to it.

Ruled out: keeping the halt and only adding the backtrace. The other CPUs would still run on, the locks would stay held, and the report would still come from the lockup detector 10 s later.

**Reference**
Zircon's `exception_die()` (`zircon/kernel/arch/x86/exceptions_c.cc:104`) calls `platform_panic_start()` first, which stops the other CPUs. It then prints the vector, the message and `dump_fault_frame()`, and ends in `platform_halt(HALT_ACTION_HALT, ZirconCrashReason::Panic)`, which records the crash and halts or reboots as configured. Linux's `oops_end()` (`arch/x86/kernel/dumpstack.c`) panics when the fault came in interrupt context or `panic_on_oops` is set.

**Acceptance**
A kernel-mode #UD (for example a `ud2` behind a kernel shell command on dbg) prints the banner, the exception, its registers and a backtrace that names the faulting function and its caller, and resets under `kernel.panic.reboot`, with no `lockup:` line. `ours-verify` passes on both images.
