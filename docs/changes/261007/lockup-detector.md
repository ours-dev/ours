# lockup-detector changes

## An NMI that nothing in the kernel sent dumps every CPU's registers and backtrace once, then panics (2026-10-07)

**Files**
- `kernel/lib/lockup_detector/` — new library: `include/lockup_detector/lockup_detector.hpp` (`on_nmi()`, the arch hook `arch_dump_registers()`), `src/report.cpp` (the claim, the request mask, one context slot per CPU, the target's capture and park, the reporter's wait, print and panic), `BUILD.gn`, `README.md`; scaffolded with copier from `tools/templates/cpp_library`, its placeholder host test left out until step 2 brings a pure function.
- `kernel/arch/x86/kernel/faults.cpp` — the NMI case hands every NMI to `lockup_detector::on_nmi()` instead of dropping it; the five register lines move out of `panic_exception()` into `log_registers()`, which also defines `lockup_detector::arch_dump_registers()`.
- `kernel/arch/x86/kernel/mp.cpp`, `kernel/arch/x86/kernel/include/ours/arch/mp.hpp` — `arch_mp_send_nmi()`: one bounded NMI send per target, returning the targets it gave up on.
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — `XApicRegType::LvtLogicalInt1` (0x360) and `XApic::route_lint1_to_nmi()`.
- `kernel/arch/x86/apic/local_apic.cpp` — `init_apic_lint1()`: logs what the firmware left in the boot CPU's LINT1, then routes LINT1 to NMI, unmasked on the boot CPU and masked on the others.
- `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` — `IrqFrame::irqs_enabled()`, RFLAGS.IF of the interrupted code.
- `kernel/top/panic.cpp`, `system/public/ours/panic.hpp` — the private `PANIC_STARTED` flag becomes `PANIC_CPU`, the CPU that claimed the panic; `panic_in_progress()` and `this_cpu_in_panic()` read it.
- `kernel/commands/lockup.cpp`, `kernel/commands/BUILD.gn` — `lockup nmi`: the CPU sends itself an NMI, as QEMU's monitor `nmi` sends one to the machine.
- `kernel/arch/x86/kernel/BUILD.gn` — the arch kernel links the detector.
- `.agents/tasks/lockup-watchdog/DESIGN.md` — Accepted, the Decided column filled, step 1's revisions under Staging, two open questions answered; `.agents/tasks/lockup-watchdog/PLAN.md` — new.
- `docs/repairs/261007/kernel-smp.md` — the silent-hang item says a silent boot can now be dumped.

**Symptom**
A hung machine said nothing. About one dbg boot in a dozen stops dead in the shell's fault-probe phase, every CPU silent within 16 ms, no panic and no error line (`docs/repairs/261007/kernel-smp.md`, item 1), and thirty boots under a debugger did not catch it again. There was no way to ask a hung guest what each CPU was doing: QEMU's monitor `nmi` did reach the guest, but the kernel threw the NMI away.

**Cause**
Three things were missing, and each one alone was enough to keep a hang silent.

- **The one interrupt a stuck CPU still takes was dropped.** A CPU spinning with interrupts off (every spin lock is held and waited for with IF clear) accepts no maskable interrupt at all: a tick, a reschedule IPI, the suspend IPI the panic path sends. Interrupt acceptance requires RFLAGS.IF set; an NMI does not. It is held off only while the CPU is already inside an NMI, from delivery until the next `iretq`. So the NMI is the only way in, and the NMI vector was `break;`, on purpose, because the handler had nothing it could safely do. It also meant the hardware path already worked and the kernel threw it away: the boot CPU's LINT1 already reads 0x400 (NMI delivery, unmasked) when the kernel first looks, as the firmware left it, and QEMU's `nmi` delivers through each CPU's LINT1, so the monitor's `nmi` had always reached cpu0's NMI vector and been dropped there.
- **Nobody could print from an NMI.** The NMI lands at any instruction, including inside `SerialLogger::write()` on the same CPU, with the console lock held. A handler that logs there waits for a lock whose holder is the code it interrupted, which cannot run again until the handler returns: a self-deadlock, not a slow path. Four CPUs printing at once from their NMIs would also interleave line by line, since nothing serialises them without that lock. So the handler on a stuck CPU must take no lock and print nothing, and the printing has to happen elsewhere, by one CPU, in a mode where the console lock is only waited for a bounded time: logz4's panic mode, where `SerialLogger::acquire()` gives up after `PANIC_LOCK_TICKS` and writes anyway.
- **The kernel had no way to send an NMI, nor to say which NMI was a request.** The IPI paths all send Fixed delivery, and the unbounded `send_ipi()` waits for the ICR to go idle forever, which is the wrong thing to do on a machine that is already stuck.

The protocol that came out of those constraints (the DESIGN's D5, D6, D7 and D9), as objects and events:

- Objects: `REPORTER`, the one CPU that prints, claimed once by compare-exchange; `REQUESTED`, the mask of CPUs the reporter has sent an NMI to; `SLOTS`, one per CPU, each written only by its own CPU in its own NMI.
- Events: (1) an NMI that is not in `REQUESTED` for this CPU is a request for a dump, and its CPU claims `REPORTER`; (2) the reporter captures its own frame, stores the other online CPUs into `REQUESTED` and sends each an NMI; (3) each target captures its frame, thread, held spin-lock count and frame-pointer walk into its slot, clears its Online bit, publishes the slot and halts without returning from the NMI; (4) the reporter waits at most 2 s of TSC for the slots, enters panic mode, prints one block per CPU and panics.

Why each piece of the concurrency is the way it is:

- **The slot's Ready bit is a release, read with acquire.** The slot's fields are plain stores made by the target; the reporter must see all of them once it sees Ready. Release/acquire on that one atomic is what orders the plain stores before it, the same "one atomic, the state; the other fields published by its release store" shape the sampler uses.
- **`REQUESTED` carries no data, so its load is relaxed; its store is seq_cst for the fence.** The target only needs its bit; what the reporter wants back comes through the slot. What does matter is that the mask is visible before the NMI arrives: the store must reach memory before the ICR write that sends the first NMI. A seq_cst store on x86 is an `xchg`, which drains the store buffer, so no CPU can take the NMI and still read the old mask.
- **The target clears Online before it publishes**, the reverse of the DESIGN's event 7. The reporter's own panic runs `suspend_others()`, which sends a Fixed IPI to every CPU still Online and waits up to ten million spins for them to leave the mask; a parked target can never take that IPI. Clearing first means a reporter that has seen every slot also sees every target gone, so the panic waits for nobody.
- **The target never returns.** An `iretq` would unblock NMIs and resume the stuck code under the report (D7); halting inside the NMI keeps the machine still while the reporter prints, and NMIs stay blocked because the `iretq` never comes.
- **The claim is relaxed.** All read-modify-writes of one atomic are totally ordered, so exactly one compare-exchange wins; nothing is published through it. A CPU that loses it (an NMI from outside raced the reporter's request) answers as a target.
- **A target in a panic of its own goes back to it.** The reporter stands down for a panic in progress, but only for one it can see when it looks: a CPU may claim a panic after that look and before the reporter's NMI reaches it, and the NMI then lands inside that panic (in its `quiesce_machine()`, typically, waiting for the other CPUs to stop). Parked there, it took the panic with it. The reporter's own `panic()` at the end of the dump then found the panic claimed, put its message in the record ring only and stopped, as every second panic does: no banner, no message of the real panic, no crash record, no reset, only the dump. So the panic claim records which CPU took it (`PANIC_CPU`, a `CpuNum` claimed by compare-exchange, in place of the `PANIC_STARTED` flag), and a target that finds itself the panicking CPU (`this_cpu_in_panic()`, which reads its own store in program order) fills its slot, publishes it with `Ready | Panicking` in one release store, and returns from the NMI to its panic. Its panic waits, within its bound, for the reporter to leave the Online mask, which the reporter's own stopped panic does once the dump is out; the panic's report then follows the dump. The slot is filled only once: the `iretq` back to the panic unblocks NMIs, and a later NMI must not rewrite a slot the reporter may be reading.
- **The stack walk is bounded to the interrupted thread's kernel stack**, from the interrupted sp to the stack's top (`sampler::StackWindow`), because the walk follows frame pointers it cannot trust and a fault inside an NMI cannot be taken. A user frame is not walked at all.

LINT1 is unmasked on the boot CPU only. One CPU taking the outside NMI is enough, since the report then reaches the others through NMI IPIs; if every CPU took it, each would contend for the claim and all but one would answer unasked.

**Change**
The NMI vector, before:

```cpp
case IrqVec::Nmi:
    // Deliberately silent. ...
    break;
```

and after:

```cpp
case IrqVec::Nmi:
    // The lockup detector's, whoever sent it: ...
    lockup_detector::on_nmi(*frame);
    break;
```

The hook, in `src/report.cpp`:

```cpp
auto on_nmi(arch::IrqFrame const &frame) -> void {
    auto const self = ours::Cpu::id();
    if (REQUESTED.load(std::memory_order_relaxed).test(self)) {
        if (ours::this_cpu_in_panic()) {
            answer_from_panic(frame, self);   // capture, publish as Panicking, back to the panic
            return;
        }
        answer(frame, self);              // capture, leave Online, publish, park
    }
    if (ours::panic_in_progress()) {
        return;                           // a panic is already printing its report
    }
    auto reporter = ours::INVALID_CPU_NUM;
    if (REPORTER.compare_exchange_strong(reporter, self, std::memory_order_relaxed)) {
        report(frame, self);              // send, wait, print, panic
    }
    if (reporter == self) {
        return;
    }
    answer(frame, self);
}
```

The send, beside `arch_mp_suspend()`, through the bounded `try_send_ipi()` whose comment had reserved it for NMI delivery:

```cpp
auto arch_mp_send_nmi(CpuMask const &targets) -> CpuMask {
    if (XAPIC == nullptr) {
        return targets;
    }
    auto unsent = CpuMask{};
    targets.for_each([&unsent](CpuNum cpu) {
        if (!apic_try_send_ipi_unicast(cpu, IrqVec::Nmi, ApicDeliveryMode::NonMaskable)) {
            unsent.set(cpu);
        }
    });
    return unsent;
}
```

LINT1, written in `init_local_apic_percpu()` on every CPU: `route_lint1_to_nmi(masked)` writes delivery mode NMI (0x400) and the mask bit on every CPU but the boot one; the boot CPU first logs the firmware's value. The report's lines, one block per CPU:

```text
lockup: dump asked for by an NMI on cpu0; each CPU's state follows
lockup: cpu0 (reporting) thread kshell at +0x18cf2, interrupts off, 0 spin locks held
  rip=0xffffff8840018cf2 (+0x18cf2)  cs=0x10  rsp=0xffffff880404c8b0
  rax=... (four more register lines, as a kernel fault prints them)
lockup: cpu0 backtrace +0x18cf2 +0x168aa +0x84b81 +0x1a9ff +0x9eb6f +0x26a7c
```

Choices made on the way, each recorded in DESIGN.md Staging step 1 ("Revised"): the namespace is `lockup_detector`, the directory's name, not the DESIGN's `lockup`; the register lines are a third arch hook, `arch_dump_registers()`, because the names are x86's, and `faults.cpp` shares them with `panic_exception()` rather than keeping two copies; the slots are a plain array indexed by CPU rather than `CPU_LOCAL` data, so the reporter needs no per-CPU unit to find one; `arch_mp_send_nmi()` reports the CPUs it gave up on, which the report names apart from a CPU that did not answer; `lockup nmi` sends its own CPU an NMI so that it walks the same path as the monitor's; and the reporter writes a bare newline first, because a target parked in the middle of a log line leaves the console there (the first final run showed the header glued to cpu2's half-written `I'm Thread 1`).

Rejected on the way: printing from each target's NMI (the self-deadlock and the interleaving in Cause); a Fixed-vector IPI for the request (a CPU with IF clear never takes it); waiting for the targets with a spin count (one spin costs about 640 ns under TCG, so the bound has to be time: `ANSWER_WAIT_TICKS` is 2 s at the 1 GHz TSC this QEMU gives the guest, measured at boot as `TSC 999984993 Hz`).

**Reference**
- Linux: `nmi_trigger_cpumask_backtrace()` (`lib/nmi_backtrace.c`) sets a backtrace mask, raises the NMIs through `nmi_raise_cpu_backtrace()`, which is `__apic_send_IPI_mask(mask, NMI_VECTOR)` (`arch/x86/kernel/apic/hw_nmi.c`), and waits up to `NMI_BT_TIMEOUT_SEC` for the mask to empty; each target prints its own registers from the NMI in `nmi_cpu_backtrace()`, serialised by `printk_cpu_sync_get_irqsave()`, and returns. Ours has no counterpart of `printk_cpu_sync`, so the target captures and the reporter prints (D6), and the target parks (D7). LINT1 follows `setup_local_APIC()` (`arch/x86/kernel/apic/apic.c`): "Only the BSP sees the LINT1 NMI signal by default", `APIC_DM_NMI` on CPU 0 and `APIC_DM_NMI | APIC_LVT_MASKED` elsewhere. An NMI nobody claims goes to `unknown_nmi_error()` (`arch/x86/kernel/nmi.c`), which panics through `nmi_panic()` only with `unknown_nmi_panic`; ours always dumps and panics, since an unclaimed NMI here is a request. The panic claim as a CPU number follows `panic_cpu` (`kernel/panic.c`, claimed by `atomic_try_cmpxchg()` in `panic()` and `nmi_panic()`) and its question `this_cpu_in_panic()` (`kernel/printk/printk.c`); Linux needs no exception for the panicking CPU in `nmi_cpu_backtrace()` because its targets always return from the NMI, while ours park.
- Zircon: `x86_nmi_handler()` (`kernel/arch/x86/exceptions_c.cc`) calls `CpuContextExchange::HandleRequest()` (`kernel/lib/backtrace/include/lib/backtrace/cpu_context_exchange.h`), which captures the frame and a frame-pointer backtrace into one storage slot only when `target_cpu_` names this CPU, signals with a release store, and returns; `RequestContext()` asks one CPU at a time under its own spin-like claim, with a timeout. Zircon drops an NMI nobody asked for and never programs LINT1. Ours takes the capture-into-a-slot shape, with a slot per CPU so that every CPU is asked at once.

**Verification**
- Build of `out/bringup.x86` in `.worktrees/feat-lockup-watchdog`: ok, `-Werror`. `scripts/format_cpp.py --check` clean on all eleven changed C++ files; `scripts/format_gn.py` clean on the three GN files. `clang-tidy -p out/bringup.x86` on `report.cpp`, `lockup.cpp`, `faults.cpp`, `mp.cpp`, `local_apic.cpp` and `panic.cpp`: nothing on a changed line (what it reports is in headers this change does not touch, or on their old lines).
- `boot_check.py` on the tree as committed: dbg PASS (shell finished at 52.7s; 35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required), host-tests 142 passed; rel.log has `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`; no `lockup:` line in either log; both logs have `local APIC: the firmware left LINT1 at 0x400; routing it to NMI`.
- `lockup nmi` on dbg, typed after `--- end ---`, with `REBOOT=1` (a scratch driver like `kshell_run.py`'s console, since `kshell_run.py` learns `lockup` only in step 3): one header, one block for each of cpu0-cpu3, `OURS KERNEL PANIC` once, the 32-line tail, the reset; the next boot logged `crashlog: the previous boot panicked; its record (2783 bytes) follows`, `crashlog: | lockup: dump asked for by an NMI on cpu0` with every CPU's block behind it, and `userboot: the previous boot's crash record: lockup: dump asked for by an NMI on cpu0`. The four blocks fit the tail the record keeps (D14 (a)). Symbolized by hand against `kernel_x86.dbg.1/obj/kernel/products/bin/vmours` with `llvm-nm -n`: the reporter's walk is `apic_try_send_ipi_unicast+0x62`, `arch_mp_send_nmi+0x4a`, `lockup_command+0x101`, `gktl::run_command+0x7ff`, the kshell thread, `Thread::trampoline+0x15c`, which is the command's own call chain; cpu1 and cpu3 were in `SerialLogger::write` with interrupts off (the dbg image's `I'm Thread` loops); cpu2 was idle in `ArchIdleThread::idle+0x2`, through `become_idle`, `start_nonboot_cpu` and `x86_start_nonboot_cpu`.
- QEMU's monitor `nmi`, sent through a monitor socket after `--- end ---`: on dbg, the same report, from cpu0 (`thread Idle/0`), with cpu1-cpu3 answering its NMI IPIs; on rel, the same, cpu0 in the user shell's `debug_console_read` system call (`debug_console_read+0x22`, `debug_read_shim`, `syscall_dispatch`, `x86_syscall_dispatch_return`), and rel halts after the panic, as its cmdline says. cpu0 reported both times, the one CPU whose LINT1 is unmasked; the report cannot tell an AP's answer to cpu0's NMI IPI from an AP that took the outside NMI too and lost the claim, so that part rests on the LINT1 masks, not on the output.
- The NMI stack, measured once with temporary code not committed (each CPU's NMI stack painted with 0xA5 in `x86_setup_tss_percpu()`, scanned after `record_panic()`): the reporter used 3896 of 16384 bytes on dbg, for `lockup nmi` and for the monitor's `nmi` alike, and 3368 on rel; each target 1124 on dbg and 576 on rel. The reporter's figure covers the report and the whole panic up to the crash record.
- The cost of a console lock held by a parked CPU, as the DESIGN expects: when a target was parked inside `SerialLogger::write()`, the report's first write waited the 250 ms `PANIC_LOCK_TICKS` bound (header at 44.545 s, first block at 44.795 s), and every later line went straight out.
- Boots run for this step: 13 QEMU instances, never two at once outside `boot_check.py`'s own pair. Three experiments on the build with the stack measurement (dbg `lockup nmi` with a reboot, dbg monitor `nmi`, rel monitor `nmi`), three `boot_check.py` runs of both images (the last after two comment-only edits, on the tree as committed), two dbg `lockup nmi` runs with a reboot (before and after the leading newline), and one monitor `nmi` each on dbg and rel on the final code.
- The panic that a request crosses, reproduced in review with temporary code not committed: a `lockup race` command on the kshell's CPU sent cpu1 an NMI, waited 50 ms and called `panic()`, and `on_nmi()` waited 300 ms between its panic check and its claim. Before the fix (dbg, `REBOOT=1`): the dump printed the reporter and the panicking kshell CPU (parked in its own `quiesce_machine()`), then nothing: no banner, no `lockup race test` message, no reset within 45 s. After it: the dump names `cpu0 (in a panic of its own, reported after this) thread kshell`, then `OURS KERNEL PANIC`, `lockup race test: cpu0 panics while cpu1 reports`, the tail with the reporter's `panicked while another panic was under way: lockup: dump asked for by an NMI on cpu1`, the reset, and the next boot's `crashlog: | lockup race test: ...` with the dump inside the record.
- Review on 2026-10-07, after the fix with the temporary code removed: `boot_check.py` dbg PASS, rel PASS (`pc-ps2: key a`, `pc-ps2: key A`), host-tests 142 passed, no `lockup:` line; `lockup nmi` on dbg with `REBOOT=1` gave one header, cpu0-cpu3 once each, `OURS KERNEL PANIC` once, the reset and the next boot's crash record. Before the fix, on the step's own commit, the same `boot_check.py`, `lockup nmi` on dbg and the monitor's `nmi` on rel had passed the same way, and after a rebase onto `cfd8f012` (host tests only) `boot_check.py` passed once more. Boots in review: 11 QEMU instances, never two at once outside `boot_check.py`'s pair.

**Deliberately not done**
- No heartbeat and no checker: a CPU stuck alone is not found by itself until step 2. Today a hang reports only when something sends an NMI.
- No change to `kshell_run.py` or `boot_check.py`: `lockup nmi` ends the machine without a prompt, and teaching the scripts that, and sending `nmi` on a silent boot, is step 3. The runs above used a scratch driver.
- No symbolizer: the report prints image offsets, read with `llvm-nm`/`llvm-objdump` on the host. A leaf function's caller is lost to the frame-pointer walk (D10 (a)); rel showed it, a sample thread in `strlen+0x0` with `Thread::trampoline` as the next frame.
- The boot CPU's idle thread gets no backtrace: it runs on the boot stack, not on the stack its Thread records, so the window is empty and the block keeps the pc alone. Not chased: an idle CPU's backtrace says nothing a hang needs.
- A CPU interrupted inside an interrupt handler is walked through the handler's frames into the interrupted code, since the entry keeps %rbp; a CPU interrupted on an IST stack (inside #DF or #MC) keeps its pc alone.
- The reporter's own NMI landing between the two ICR writes of an IPI the interrupted code was sending is the DESIGN's accepted hazard; not tested.
- No host test: nothing in this step is a pure function. The template's placeholder test was dropped rather than kept empty; step 2's check brings the first real one.

**How it was found**
None. This is step 1 of the lockup-watchdog DESIGN's Staging, answering the repair item `docs/repairs/261007/kernel-smp.md` (1), whose option (a) asked for a hang that reports itself. The panicking-target exception came out of the step's review: `on_nmi()` checks `panic_in_progress()` and only then claims, and nothing stops a panic from starting in between; following that interleaving into `vpanic()`'s second-panic path showed the real panic would be stopped, and the temporary race command above showed it on dbg.

**The user's prediction**
Skipped: the user took every decision as recommended with "直接上" on 2026-10-07, skipping the predictions and the D2, D6 and D7 arguments.

**Three questions you should be able to answer**
- A CPU spins on a spin lock with interrupts off. Why does the suspend IPI the panic path sends never reach it, and what is different about an NMI?
- The target clears its Online bit and then publishes its slot. What goes wrong in the reporter's panic if the two are done the other way round, and why does the release on the Online mask make the order hold for the reporter?
- Why must a target not log from its NMI, even though logz4 has a panic mode that stops waiting for the console lock after a bound?
