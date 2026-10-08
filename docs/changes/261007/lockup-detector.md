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

## A CPU whose tick stops is found by the others' once-a-second checks and reported, without anything from outside (2026-10-07)

**Files**
- `kernel/lib/lockup_detector/src/checker.cpp` — new: the per-CPU heartbeat, each CPU's private notes on the others, the threshold, `lockup_detector::on_tick()`, and the init hook at InitLevel::Platform that arms the checks.
- `kernel/lib/lockup_detector/include/lockup_detector/heartbeat_note.hpp` — new: `HeartbeatNote`, what one CPU last saw of another's heartbeat, and `check()`, the pure decision whether that CPU is stuck.
- `kernel/lib/lockup_detector/src/report.hpp` — new, the library's own: `priv::report_claimed()` and `priv::report_stuck()`, what the checker asks of the report.
- `kernel/lib/lockup_detector/src/report.cpp` — the reporter's part split into `gather()` (own slot, request, NMIs, wait) and the printing (`print_cpus()` with a CPU to put last, `print_counts()`), shared by the NMI cause (`report_nmi()`) and the checker's (`report_stuck()`); each slot keeps its CPU's heartbeat, printed as `<n> ticks taken` on its block's first line; the comment on an NMI that lands on the reporter covers a reporter in its tick.
- `kernel/lib/lockup_detector/src/checker.hpp` — new, the library's own: `priv::this_cpu_heartbeat()`, what the report asks of the checker, so that `HEARTBEAT` stays private to `checker.cpp`.
- `kernel/lib/lockup_detector/include/lockup_detector/lockup_detector.hpp` — declares `on_tick()`.
- `kernel/lib/lockup_detector/test/` — new: the host test of `HeartbeatNote` (`BUILD.gn`, `heartbeat_note_test.cpp`).
- `kernel/lib/lockup_detector/BUILD.gn` — `checker.cpp` in the sources; `:heartbeat_note`, the one target that builds on the host; `:headers` moved under the kernel-only guard; the `tests` group.
- `BUILD.gn` — `//kernel/lib/lockup_detector:tests` in `//:default`, so `boot_check.py` runs the test.
- `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn` — `TickSched::on_tick()` calls `lockup_detector::on_tick()` first, before the Shutdown return.
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — `kernel.lockup.threshold=<seconds>`, default 10, 0 off: `BootOptions::lockup_threshold()` and `parse_count()`.
- `kernel/commands/lockup.cpp` — `lockup spin <cpu> [<ms>]`: a thread pinned to `<cpu>` turns interrupts off and spins, for `<ms>` or for good.
- `kernel/lib/lockup_detector/README.md` — what the checker does, the layout, how to read the new header.
- `.agents/tasks/lockup-watchdog/DESIGN.md` — Staging step 2 landed, with its revisions; `.agents/tasks/lockup-watchdog/PLAN.md` — step 2 done.
- `docs/repairs/261007/kernel-smp.md` — the silent-hang item's option (a) says a CPU that stops ticking now reports itself.

**Symptom**
After step 1 a hang reported only when something sent an NMI: QEMU's monitor `nmi` typed by hand, or `lockup nmi`. A CPU stuck with interrupts off while the others went on running, the shape the silent hang in `docs/repairs/261007/kernel-smp.md` most likely starts as, said nothing until someone noticed the log and sent one.

**Cause**
Nothing inside the kernel was watching for a CPU that stops. What makes such a watch possible, and why each piece is the way it is:

- **A CPU's tick stops only when interrupts are off on it, or its tick device has stopped.** Every CPU runs the periodic tick: its local APIC timer fires every 10 ms (HZ=100), through `apic_handle_timer_irq()` and `TickDevice::handle_periodic_event()` into `TickSched::on_tick()`, idle or not, since only Legacy mode is ever set. Every spin lock is held and waited for with RFLAGS.IF clear, so a CPU spinning on a lock, or holding one forever, takes no tick at all: the timer's interrupt stays pending in its local APIC. Counting the ticks each CPU takes therefore turns "is this CPU stuck with interrupts off?" into "has this number stopped moving?", which another CPU can answer by reading it. The count is bumped first in `on_tick()`, before the Shutdown return, so that every tick that arrives counts, whatever the tick then does.
- **The heartbeat is relaxed on both sides.** It has one writer, its own CPU. Its readers only ask whether it moved since they last looked; they do not read anything else through it. A load that returns a value a few ticks old answers "moved" or "not moved" one check later, and a check comes once a second, so no ordering with any other memory is needed. (A seq_cst load would order nothing that matters here, and an acquire would publish nothing, because the writer writes nothing else for the reader.)
- **Each checking CPU has notes of its own (D2 (c)).** A note is the heartbeat it saw last and how many checks in a row it has not moved. The notes live in the checking CPU's per-CPU data and are written and read only in that CPU's own tick, with interrupts off, so a note has exactly one accessor at a time: no lock, no atomic. The alternatives each add a shared writer: one note per target shared by all checkers needs a claim per target so that two checkers do not both count (and a checker that loses the claim skips the check, a gap); a buddy ring needs every CPU to agree on who checks whom while CPUs come online. With private notes, two CPUs that find the same stuck CPU in the same second meet only at the report's one claim, which was already there.
- **The unit is the checker's own check, not time (D3 (b)).** A CPU checks on every HZ-th of its own ticks, so its notes count seconds of its own running. A pause of the whole machine (a debugger stop, the host asleep, a host so loaded that every vCPU waits) stops the checker's ticks too and counts no checks. A timestamp heartbeat would age every CPU at once across such a pause and fire on resume.
- **A CPU that has never ticked is not counted.** A heartbeat of 0 has not moved since its per-CPU unit was made, so its tick has not started. That is not a lockup, and counting it would report a CPU whose timer is still being set up. The checks are also not armed until InitLevel::Platform, after `init_platform()` has brought every CPU up.
- **The checker stands down during a panic, and once a report is claimed.** A panic stops the other CPUs with an IPI, and a stopped CPU takes no tick: a check during a panic would find the panic's own work and start a second report over it. Once the claim is taken, the reporter is already gathering every CPU's state, and its NMI will park this checker too.
- **Finding a stuck CPU reuses step 1's report whole.** The checker claims the same `REPORTER` an NMI claims, then does what the NMI's reporter does: its own context into its slot (from `irq::current_frame()`, the frame its tick interrupted), the NMIs to every other online CPU, the wait, panic mode, one printer. The stuck CPU still takes the NMI, because an NMI ignores RFLAGS.IF; that is why the report's targets are reached through NMIs at all (D5).
- **The reporter may now run in its tick rather than in an NMI.** An NMI from outside can then land on it while it reports (cpu0 has LINT1 unmasked). `on_nmi()` finds the claim held by its own CPU and returns, and the tick's report goes on underneath; unlike the NMI-context reporter's case, nothing is damaged, since the tick does not run on the NMI stack.
- **Every block carries its CPU's heartbeat.** The DESIGN's context slot holds the heartbeat count, and step 1 left it to this step. A CPU fills its slot in its own NMI, or in its tick for the reporter, so it reads its own count, with a relaxed load and no lock. The checker's counts line only exists when some CPU still ticks; in a dump asked for from outside, the case where every CPU has interrupts off, the slots' counts are all that is left to say in what order the CPUs went quiet. They compare only roughly between CPUs: each counts from its own first tick, and a late tick interrupt counts once however many periods it covered (in the review's run below the healthy cpu0 was 630 behind cpu1 after 80 s), but a CPU that stopped for 18 s was 1800 behind.
- **The stuck CPU's block comes last (D14 (a)).** The crash record keeps the ring's last 32 lines, ending with the panic message. A report for four CPUs is 30 lines (the header, the counts line and seven lines per CPU); printed last, the block of the CPU the report is about is the part most sure to be kept. In the runs below the whole report fitted in the tail.

**Change**
The tick, before:

```cpp
auto TickSched::on_tick(TimePoint<> now) -> void {
    m_canary.verify<Self>();

    if (m_workmode == WorkMode::Shutdown) {
        return;
    }
```

and after:

```cpp
auto TickSched::on_tick(TimePoint<> now) -> void {
    m_canary.verify<Self>();

    lockup_detector::on_tick();

    if (m_workmode == WorkMode::Shutdown) {
        return;
    }
```

The heartbeat and the check (`checker.cpp`):

```cpp
CPU_LOCAL std::atomic<u64> HEARTBEAT;
CPU_LOCAL priv::Notes NOTES;                 // std::array<HeartbeatNote, MAX_CPU>
constinit std::atomic<u32> THRESHOLD{0};     // 0: not armed, or off

auto on_tick() -> void {
    auto const ticks = CpuLocal::access(HEARTBEAT).fetch_add(1, std::memory_order_relaxed) + 1;
    if (ticks % TICKS_PER_CHECK == 0) {      // HZ: once a second of this CPU's ticks
        check_others();                      // threshold, stand-down, notes, report_stuck()
    }
}
```

The check itself, pure and host-tested:

```cpp
[[nodiscard]]
constexpr auto check(u64 const heartbeat, u32 const threshold) -> bool {
    if (heartbeat != m_heartbeat) {
        m_heartbeat = heartbeat;
        m_unchanged = 0;
        return false;
    }
    if (heartbeat == 0) {
        return false;
    }
    ++m_unchanged;
    return threshold != 0 && m_unchanged >= threshold;
}
```

`THRESHOLD` is the checker's one atomic: 0 until the Platform hook stores `kernel.lockup.threshold`, and left at 0 when the option says 0, so there is no separate armed flag to keep in step with it. The option takes decimal digits only, up to what a u32 holds; anything else is logged and keeps the default, as a boolean option outside its grammar does.

The report's new header, from the dbg run below:

```text
lockup: cpu2 took no tick for 10 s, found by cpu0; each CPU's state follows
lockup: checks in a row without a tick, as cpu0 counted them: cpu1 0 cpu2 10 cpu3 0
lockup: cpu0 (reporting) thread Idle/0 at +0x9fda6, interrupts on, 0 spin locks held, 7200 ticks taken
...
lockup: cpu2 thread lockup-spin at +0x85bc2, interrupts off, 0 spin locks held, 6034 ticks taken
...
lockup: cpu2 backtrace +0x85bc2 +0x2753c
```

The header above is from the review's run; the runs under Verification came before the slot's heartbeat was added, so their block lines end at the spin-lock count.

`lockup spin <cpu> [<ms>]` spawns a thread with hard affinity to `<cpu>` that takes an `arch::AutoInterruptDisabler` and spins on `SteadyClock::now()` (the clock source's counter, which runs on with interrupts off), for `<ms>` or for good; the command joins it.

Choices recorded in DESIGN.md Staging step 2 ("Revised"): the snapshot is the class `HeartbeatNote`; "has moved at least once" is a heartbeat that is not 0; the increment and the check are one call; the threshold doubles as the armed state; the header is two lines; the checker reaches the claim through `src/report.hpp`; and `:headers` became kernel-only because the tests' group loads the BUILD.gn in the default toolchain, where `//kernel/lib/arch:headers` would load `//kernel/mm` and stop `gn gen` (it did, on the first build).

Rejected on the way: a separate armed flag beside the threshold (two atomics that mean one thing, the second-copy mistake the sampler made); a `const_cast` to hand `lockup spin`'s length to the thread (the parameter is taken by value and is the object whose address the joined thread reads).

**Reference**
- Linux: the heartbeat is `hrtimer_interrupts`, bumped by `watchdog_hardlockup_kick()` (`atomic_inc_return()`) from the per-CPU `watchdog_timer_fn()` (`kernel/watchdog.c`), which also returns at once when `panic_in_progress()`. The check is `is_hardlockup()`: `hrtimer_interrupts_saved` and `hrtimer_interrupts_missed`, a count of checks without a change against `watchdog_hardlockup_miss_thresh`, with `watchdog_hardlockup_update_reset()` when it moved; `watchdog_thresh` defaults to 10. The buddy detector, `watchdog_buddy_check_hardlockup()` (`kernel/watchdog_buddy.c`), runs that check from each CPU's own kick on the next CPU in `watchdog_cpus`, so it needs no NMI source, only another CPU that still ticks; ours checks every other CPU from each instead of one ring neighbour. Linux's notes (`hrtimer_interrupts_saved`) are per target, written by its one buddy; ours are per checker and per target. lkdtm's `HARDLOCKUP` (`drivers/misc/lkdtm/bugs.c`) is the test: `local_irq_disable()` and spin.
- Zircon: `DoHeartbeatAndCheckPeerCpus()` (`kernel/lib/lockup_detector/lockup_detector.cc`) stamps `last_heartbeat` from a per-CPU timer and checks every peer, taking the per-target `current_checker_id` claim first and skipping the check when it loses it; `HeartbeatLockupChecker::PerformCheck()` compares the age of the heartbeat against the oops and fatal thresholds, in time. Ours keeps the every-peer shape, but counts checks rather than time and keeps private notes rather than a shared per-target state with a claim. `k lockup test_spinlock <cpu> <ms>` runs `lockup_trigger_spinlock()` through `run_lockup_func()`: a thread pinned with `SetCpuAffinity()`, joined, holding a `MonitoredSpinLock` with IrqSave for the duration. `lockup spin` takes the same pinned-and-joined shape without a lock.

**Verification**
- Build of `out/bringup.x86` in `.worktrees/feat-lockup-watchdog`: ok, `-Werror`. `scripts/format_cpp.py --check` clean on the ten changed or new C++ files; `scripts/format_gn.py` clean on the four GN files. `clang-tidy -p out/bringup.x86` on `checker.cpp`, `report.cpp`, `lockup.cpp`, `boot_options.cpp`, `tick_sched.cpp` and `heartbeat_note_test.cpp`: nothing on these files or the headers changed with them; what it reports is in other headers.
- Host test `//kernel/lib/lockup_detector/test:heartbeat_note`: 6 tests pass (a target that ticks is never stuck; stuck on the threshold-th check without a tick and not before; one tick starts the count over; a target that never ticked is not counted; threshold 0 never finds one; stays stuck past the threshold). `boot_check.py` runs it: host-tests 143 passed (142 before).
- `boot_check.py`: dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required), host-tests PASS; rel.log has `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`; both logs have `lockup detector: a CPU that takes no tick for 10 s is reported` and no `lockup:` line.
- dbg, one boot through a scratch driver built on `kshell_run.py`'s `Console` (the script learns `lockup` in step 3): `lockup spin 2 8000` logged `lockup: cpu2 spins with interrupts off for 8000 ms` at 51.121 s and `lockup: cpu2 has interrupts on again` at 59.279 s, and no `lockup:` error line came in the 15 s watched after it. Then `lockup spin 2` at 74.347 s; at 84.418 s, 10.07 s later, `lockup: cpu2 took no tick for 10 s, found by cpu1`, the counts line `cpu0 0 cpu2 10 cpu3 0`, the blocks of cpu0, cpu1 (reporting), cpu3 and last cpu2 (`thread lockup-spin at +0x85b62, interrupts off, 0 spin locks held`), `OURS KERNEL PANIC` once, `lockup: cpu2 took no tick for 10 s`, the 32-line tail holding the whole report, and the reset. Symbolized with `llvm-nm -n -C` on `kernel_x86.dbg.1/obj/kernel/products/bin/vmours`: cpu2 at `spin_routine(void*)+0x32` under `Thread::trampoline()+0x15c`; cpu0 and the reporter in `ArchIdleThread::idle()+0x2`, the reporter's tick having interrupted its idle thread with interrupts on; cpu3 in the Sample2 thread's logging.
- rel has no kernel shell (`kernel.shell=false`), so temporary code, not committed, called `lockup spin 2` through `gktl::run_command()` 15 s after InitLevel::Platform. The spin began at 18.627 s; at 29.345 s, 10.72 s later, `lockup: cpu2 took no tick for 10 s, found by cpu0`, counts `cpu1 0 cpu2 10 cpu3 0`, cpu0 (reporting, in the user shell's thread), cpu1, cpu3, and last cpu2 (`thread lockup-spin at +0x70a82, interrupts off`; `spin_routine` is at 0x70a10 in `vmours.with-tests`), one banner, the tail, and the halt rel's cmdline asks for.
- `lockup nmi` on dbg on the final code: `lockup: dump asked for by an NMI on cpu0`, cpu0-cpu3 in CPU order, one banner, as in step 1.
- Review, 2026-10-07: the slot's heartbeat (`src/checker.hpp`, `priv::this_cpu_heartbeat()`, the `ticks taken` field) and `[[nodiscard]]` on `HeartbeatNote::check()` were added. `format_cpp.py --check` clean and `clang-tidy -p out/bringup.x86` quiet on `checker.cpp`, `report.cpp` and the test. `boot_check.py`: dbg PASS (35 passed, 3 skipped), rel PASS (33 passed, 9 skipped, 6/6 required), host-tests PASS (143); rel.log has `pc-ps2: key a` and `pc-ps2: key A`; no `lockup:` line in either log. On dbg, one boot: `lockup spin 2 8000` returned after 8.26 s and the 15 s after it held no `lockup:` or panic line; `lockup spin 2` was reported 10.31 s (host time) after it was typed, `cpu2 took no tick for 10 s, found by cpu0`, counts `cpu1 0 cpu2 10 cpu3 0`, cpu2 last at `+0x85bc2` (`spin_routine(void*)+0x32` by `llvm-nm -n -C`) with interrupts off and 6034 ticks taken against 7831-7833 for cpu1 and cpu3, one `OURS KERNEL PANIC`, and the reset. Three QEMU instances: the `boot_check.py` pair and the dbg boot.
- Boots run for this step: 7 QEMU instances, never two at once outside `boot_check.py`'s own pair: two `boot_check.py` runs of both images (the first after the code was written, the last on the tree as committed), one dbg boot for the two spins, one rel boot with the temporary trigger, one dbg boot for `lockup nmi`.

**Deliberately not done**
- No `lockup spin all`, and no catching of a hang in which every CPU has interrupts off: no CPU ticks, so no checker runs. That is D4's documented gap; the scripts sending `nmi` on silence are step 3.
- No `lockup status` and no measurement of the margin under host load: step 4. Today's threshold of 10 rests on Linux's `watchdog_thresh` and on the runs above, where the healthy CPUs' counts were 0 when the report came.
- `lockup spin` holds no spin lock while it spins, so its block says 0 spin locks held. Holding one would show the count working, but the report already prints it, and a lock held for good would also stop any other CPU that wants it, which is the every-CPU case of step 3, not this one.
- The heartbeat is a locked `fetch_add` (`lock xadd` on x86) though it has one writer: a plain load and store of the atomic would do and cost less. At 100 increments a second per CPU the difference is not measurable, and the working rules put performance work to the user first.
- No change to `kshell_run.py`: it waits for a prompt that `lockup spin` without a length never gives back. The runs above used a scratch driver; step 3 teaches the script.
- When tickless idle comes, an idle CPU stops ticking and would look stuck. The DESIGN's open question stays open: the checker would then skip idle CPUs, or the heartbeat would become a timer (D1 (b)).

**How it was found**
None. This is step 2 of the lockup-watchdog DESIGN's Staging.

**The user's prediction**
Skipped: the user took every decision as recommended with "直接上" on 2026-10-07, skipping the predictions and the D2, D6 and D7 arguments.

**Three questions you should be able to answer**
- Why does a CPU spinning on a spin lock stop moving its heartbeat, while a CPU spinning in `sync_exec()` waiting for an IPI answer does not?
- Every CPU checks every other on notes of its own. What would two checkers have to agree on if they shared one note per target, and what goes wrong in the worst interleaving if they do not?
- The threshold counts the checking CPU's own checks rather than seconds of wall time. What happens to each kind of threshold when the host stops the whole VM for 30 s and then resumes it?
