# kernel-irq changes

## The interrupt entry is the root controller's handler: arch calls the irq layer, and the controller's driver ends what no line has (2026-11-01)

**Files**
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `irq::RootHandler`, `irq::set_root_handler()` and `irq::handle_root_irq()`; `handle_irq_preamble()` loses the number nothing read, and both bracketing calls are documented as arch's, once per trap.
- `kernel/platform/pc/interrupt.cpp` — `handle_root_interrupt()`, the PC's root handler, installed by `platform_init_apic()` after the lines are built and before interrupts are enabled; the three forwarders `platform_handle_irq()`, `platform_start_handling_irq()` and `platform_finish_handling_irq()` are gone.
- `kernel/arch/x86/kernel/faults.cpp` — `arch_handle_exception()` calls `irq::handle_irq_preamble()` and `irq::handle_irq_postamble()` itself, and a platform vector goes to `irq::handle_root_irq()`; arch sends no EOI of its own for it, and `irqvec_to_irqnum()` is gone.
- `kernel/platform/include/ours/platform/interrupt.hpp` (deleted), `kernel/platform/BUILD.gn` — the platform header had no other content.
- `kernel/arch/x86/mm/fault.cpp`, `kernel/kernel/irq/desc_table.cpp`, `kernel/include/ours/irq/desc.hpp`, `kernel/commands/interrupt_line_test.cpp` — comments that said arch calls the platform's forwarder or ends a no-line interrupt.
- `.agents/skills/ours-kernel-design/references/task.md`, `docs/development/kernel/sync/atomic-context.md` — the recorded rule and one note.

**Symptom**
Latent, cannot be triggered today: only x86 exists. It would be triggered by the first riscv or arm port, whose interrupt entry could not be written against the old seam. `.agents/tasks/irq-multi-arch/DESIGN.md` has the argument; this is its step 2.

**Cause**
Not a mistake on x86: a seam with two x86 assumptions inside it, each true there and false elsewhere, and a layering defect beside them.

- **Arch knows the number.** On x86 the vector in the trap frame is the interrupt, and the platform programmed it as `PlatformIrqMin` plus the GSI, so arch subtracted the base and passed `platform_handle_irq()` a number. A RISC-V PLIC interrupt arrives as one trap cause, "supervisor external interrupt", and the hart learns which source it is by reading its claim register. A GIC's acknowledge register does the same and also takes the interrupt. The number exists only after a read that belongs to the controller's driver, so an entry that takes a number from arch cannot be written for them.
- **Arch can end an interrupt nobody claimed.** When `platform_handle_irq()` answered false, arch sent the local APIC's EOI. That is one register write with nothing to name. A PLIC needs the claimed id written back to the claim register, a GIC needs the interrupt deactivated; arch has neither. Worse, doing nothing is not harmless: a PLIC source that is claimed and never completed stays in service in the gateway and raises no further interrupt, and an x86 vector left in service holds back every interrupt of its priority class and below on that CPU for good. So whoever reads the number must also be the one who ends the interrupt that has no line.
- **The platform forwards** (a layering defect, true nowhere in particular). `platform_start_handling_irq()` and `platform_finish_handling_irq()` took a number and called `irq::handle_irq_preamble()` and `irq::handle_irq_postamble()`; the number was the raw vector, not the GSI, and the preamble ignored it. They added a layer and a header that included an x86 header, in a directory every architecture builds.

**Change**
Arch now brackets the trap with the irq layer's own calls, and hands an interrupt that comes through the root controller to a function the controller's driver installed:

```cpp
// before, in x86's arch_handle_exception()
auto const irqnum = HIrqNum(vector);
platform_start_handling_irq(irqnum, frame);
auto const raised = x86_dispatch_exception(vector, frame, &user_exception);
auto const need_preempt = platform_finish_handling_irq(irqnum, frame);

case IrqVec::PlatformIrqMin... IrqVec::PlatformIrqMax:
    if (!platform_handle_irq(irqvec_to_irqnum(vector), frame)) {
        apic_issue_eoi();
    }
    break;

// after
irq::handle_irq_preamble(frame);
auto const raised = x86_dispatch_exception(vector, frame, &user_exception);
auto const need_preempt = irq::handle_irq_postamble();

case IrqVec::PlatformIrqMin... IrqVec::PlatformIrqMax:
    irq::handle_root_irq(frame);
    break;
```

and the PC's handler, which owns both the vector-to-GSI rule it programmed and the EOI for a vector no line has:

```cpp
auto handle_root_interrupt(arch::IrqFrame *const frame) -> void {
    DEBUG_ASSERT(frame->vector >= arch::IrqVec::PlatformIrqMin);
    auto const gsi = static_cast<HIrqNum>(frame->vector) -
                     static_cast<HIrqNum>(arch::IrqVec::PlatformIrqMin);
    if (!irq::handle_irq_generic(gsi)) {
        apic_issue_eoi();
    }
}
```

`set_root_handler()` stores the function in a `std::atomic` once (a second call, or a null one, is a kernel bug and stops), and `handle_root_irq()` loads it; an interrupt that arrives before one is installed stops too. What keeps a CPU from reading the pointer before it is written is the boot order, not the atomic: the registration is in the Irq-level start-up hook on the boot CPU, which has interrupts off until the `arch::local_irq_enable()` that ends it, and the other CPUs start after that level. The release store and the acquire load make the line table, built by `irq::init_lines()` just before, visible to whoever reads a handler.

Rejected, by the user's decision D3 of 2026-11-01 (it chose (c) over the recommended (a)): a numberless `platform_handle_irq(frame)` that stays in a platform header. It would also have moved the EOI, but would have kept a function in a header every platform implements; (c) lets the controller's driver install the entry, and the header goes. A second entry shaped like a claim (b) was not taken either: two shapes for generic code to support.

The rule of `task.md` ("arch sends it only for a vector with no line") changes with it: the root handler sends it. That is a recorded genirq detail, and the user's choice of (c) is the approval.

**Reference**
Linux `set_handle_irq()` (`arch/arm64/kernel/irq.c`, from memory, unchecked) is the shape: a driver registers the function arch's entry calls. On arm64 the function registered is `gic_handle_irq()` in `drivers/irqchip/irq-gic-v3.c`, which deactivates an interrupt nothing handled. On riscv it is the per-hart interrupt controller's `riscv_intc_irq()` in `irq-riscv-intc.c`, and the PLIC's `plic_handle_irq()` in `irq-sifive-plic.c` is chained under it by `irq_set_chained_handler()`; it only warns when nothing handles the interrupt, which is the wedged source above. Ours keeps the cause dispatch in arch and puts the PLIC's claim in the root handler. These were read from Linux master by a review agent on 2026-11-01 and are not kept in the tree. Zircon, the shape not chosen: `platform_irq(frame)` with `int_handler_start()` and `int_handler_finish()` called by arch, as `.agents/tasks/arch-platform-seams/DESIGN.md` records them (that document exists only on `origin/feat/riscv`, `git show origin/feat/riscv:.agents/tasks/arch-platform-seams/DESIGN.md`; cited there, not re-read).

**Verification**
- `boot_check.py` (the build tree `out/bringup.x86`): dbg PASS, 44 passed and 4 skipped; rel PASS, 41 passed and 8 skipped, 6 of 6 required lines, among them `pc-ps2: key a` and `pc-ps2: key A`, which are a real device interrupt through the new handler; host tests 154 passed. Before the change: the same numbers.
- The kernel-shell tests of the layer, on dbg, all ok: `irq.test` 72 checks, `interruptline.test` 59, `pinlines.test` 28, `irqdomain.test` 39, `interrupt.test` 32. `interruptline.test` holds the case that a vector no line has, raised by IPI, is ended and does not hold back the line after it.
- clang-tidy on `mod.cpp`, `pc/interrupt.cpp` and `faults.cpp`: no finding in the changed files (the warnings it prints are in `ustl` headers).
- Four mutations, each restored byte for byte, each killed: the no-line EOI removed (`interruptline.test` fails 2 of 59 checks); the GSI computed one too high (the boot goes silent and the NMI report ends the run); no preamble (dbg does not reach `--- end ---`); the root handler never called (rel does not reach its required lines; a lockup report).
- Not verified: the stops themselves (a second `set_root_handler()`, a null handler, an interrupt before installation, the `DEBUG_ASSERT` on the vector), which no test or mutation reaches; arm and riscv, which nothing in this tree's gate builds, so that the entry works for a PLIC or a GIC is the design's argument and not a run; that no root-controller interrupt can arrive between `init_lines()` and the registration is by reading (every pin is programmed masked, and nothing unmasks one before a driver does, later).
- Not measured: the cost of the atomic load and the indirect call that every device interrupt now takes where it took a direct call.

**Deliberately not done**
- No riscv or arm handler, no claim loop (one interrupt per trap, as x86 has), no per-CPU lines in the layer: `.agents/tasks/irq-multi-arch/DESIGN.md` D2 and the "Interrupt entry" section.
- The chip contract (step 3), the ack signal on a virtual interrupt (step 4) and AccessDenied for a kernel-owned source (step 5) are separate steps.
- `docs/repairs/261003/x86-apic.md` #2 and `docs/repairs/260926/kernel-task.md` #3 are on this path and stay their own changes. `irq::init()`, still empty, is left for refactor item R17.

**How it was found**
None: a design step. The need came from the riscv plan, whose step 2 writes a trap vector and whose PLIC gives the number only on a claim.

**The user's prediction**
None. The user chose option (c) for D3 on 2026-11-01; the recommendation was (a).

**Three questions you should be able to answer**
- Why can arch not send the EOI for an interrupt that no line has on a PLIC, when it can on x86?
- What happens to a PLIC source that is claimed and never completed, and to an x86 vector that is never ended?
- Why is the root handler installed after `irq::init_lines()` and before `arch::local_irq_enable()`, and what stops a second CPU from reading the pointer before it is written?
