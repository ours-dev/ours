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

## The chip contract is written down, a Oneshot action needs a mode or a chip that can say one, and `set_affinity` stops saying yes (2026-11-01)

**Files**
- `kernel/include/ours/irq/chip.hpp` — the contract every controller's chip keeps to, in the comment of `Chip`; `trigger_mode` and `set_affinity` documented.
- `kernel/kernel/irq/desc.cpp`, `kernel/include/ours/irq/desc.hpp` — `Desc::attach(Action &)`, the attach with no mode, refuses a Oneshot action when the chip has no `trigger_mode`.
- `kernel/kernel/irq/mod.cpp`, `kernel/include/ours/irq/mod.hpp`, `kernel/arch/x86/apic/io_apic.cpp`, `kernel/kernel/time/tick_device.cpp` — `irq::set_affinity()` and the IO-APIC chip's `set_affinity` answer NotSupported; two comments that called the stub a yes.
- `kernel/commands/irq_chip_test.cpp`, `kernel/commands/BUILD.gn` — new kernel-shell test `irqchip.test`, with no x86 type in it.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md` — step 3 done, D8 decided.

**Symptom**
Latent, cannot be triggered today. A Oneshot action attached with no mode to a line whose chip cannot say how its pins signal would be taken, and the line would run the edge flow: a level source that the owner expects to stay masked until its ack would be ended and unmasked at once, and raise the same interrupt again, for good. No caller does it. And `irq::set_affinity()` answered Ok for every line while moving none, so a caller that believed it would have thought a line aimed at a CPU it was not on.

**Cause**
Two small lies, and a contract that was one paragraph of x86.

- **The edge flow does not hold a line.** A line takes its first flow when it is bound to its chip, from `trigger_mode()`, and an edge flow when the chip has none (`Desc::init()`). Attaching with a mode calls `configure()` and chooses the flow again from the mode just set, and attaching with no mode and no way to read one answers NotSupported (`desc.cpp`, the attach with `ustl::none()`). The attach with no mode at all, `attach(Action &)`, did neither: it linked the action on whatever flow the line already had. For a Oneshot action that is the wrong flow, since only the level flow holds a line masked until the ack (the edge flow of a Oneshot line is "no hold" by design, and right for an edge pin). The first reading of this, that a PLIC would storm through the three-argument attach too, was refuted by the review of the design: only this path was open. A controller with no mode register (the PLIC) is the case it matters for; the IO-APIC reads its pins, so nothing on x86 meets it.
- **A stub that says yes.** `irq::set_affinity()` returned Ok and did nothing, and so did the IO-APIC chip's operation, which the layer never calls. The only caller, the tick device, discards the answer, which is why nobody noticed; its comment even leaned on the lie ("a stub that says yes to everything, so the clause would be vacuous"). A real caller that acts on Ok has been told something false.
- **The contract was x86's.** The paragraph on `Chip` said where `mask`, `unmask` and `configure` run, in the terms of one controller. A porter writing a chip for a PLIC or a GIC could not tell from it where `issue_eoi` may run, what `configure` does on a controller with no mode register, how a chip with several instances finds its own, or that a controller whose mask takes effect on another CPU later cannot hold a Oneshot line.

**Change**
The attach with no mode refuses what it cannot serve:

```cpp
// after the check_oneshot() call in Desc::attach(Action &)
if (action.is_oneshot() && (m_data.chip == nullptr || !m_data.chip->trigger_mode)) {
    return Status::NotSupported;
}
```

The chip is fixed before the line is published, so it is read without the lock, as `mask()` reads it. The stubs:

```cpp
// before
auto set_affinity(VIrqNum, const CpuMask &targets) -> Status { return Status::Ok; }
// after
auto set_affinity(VIrqNum, CpuMask const &) -> Status { return Status::NotSupported; }
```

and the IO-APIC chip's own operation the same. The contract in `chip.hpp` says: where each operation runs (`mask`, `unmask`, `configure` under L1 from interrupt context or a thread with interrupts off; `issue_eoi` in interrupt context, under L1 unless the line is Permanent; none sleeps or waits for another CPU); that a chip keeps nothing in the line's data but may keep a table of its own, atomic because `trigger_mode()` reads it without L1; that `issue_eoi()` runs on the CPU that took the interrupt and is never deferred; that `configure()` refuses a mode the controller cannot do and leaves the source masked, and that a controller with no register for the mode records it; that a chip with several instances finds its own from the hardware number; and that a controller whose mask is deferred to another CPU leaves `unmask()` null, which refuses a Oneshot action. Rejected: a chip flag for the deferred mask (MSI is a non-goal, and the flag is MSI's to add); the line owning its mode instead of the controller (not needed, the refutation above); a refusal inside `check_oneshot()` itself, which the attach with a mode shares and which must accept a chip that cannot say its mode when a mode comes with the action.

**Reference**
None, this project's own design: it follows from how the flow is chosen (irq-genirq D8, `.claude/skills/ours-kernel-design/references/task.md`) and from what the Oneshot hold needs.

**Verification**
- `boot_check.py`: dbg PASS, 44 passed and 4 skipped; rel PASS, 41 passed and 8 skipped, 6 of 6 required lines; host tests 154 passed. The same numbers as before the change.
- `irqchip.test`, new, 18 checks, all ok: a Oneshot action with no mode is refused by a chip that cannot say its mode and not left on the line; a plain one is taken; after the refusal the same line takes a Oneshot action that brings a mode, the chip is told the mode once and the line runs the flow of that mode; a chip that can say its mode takes a Oneshot action with no mode, on the flow it says; no mode to give and none to read is refused; `set_affinity` answers NotSupported. The other five kernel-shell tests of the layer unchanged and ok: `irq.test` 72 checks, `interruptline.test` 59, `pinlines.test` 28, `irqdomain.test` 39, `interrupt.test` 32.
- clang-tidy on the five changed sources: no finding in the changed files.
- Four mutations, each restored byte for byte, each killed by `irqchip.test`: the refusal removed (fails 5 of 18 checks); every Oneshot attach with no mode refused (fails 2); the refusal inverted (fails 7); `set_affinity` answering Ok (fails 1).
- Not verified: a chip with no `trigger_mode` that is bound in the real line table, since none exists (the PLIC chip is the riscv branch's), so the refusal's real customer is not exercised; the IO-APIC chip's `set_affinity`, which nothing calls, so changing it is covered by no test; the rule that a deferred-mask chip leaves `unmask()` null, which is a sentence and not a check.

**Deliberately not done**
- No PLIC or GIC chip, no chip flag for a deferred mask, no `platform_data` user, no line-owned mode: `.agents/tasks/irq-multi-arch/DESIGN.md`, "The chip contract".
- The tick device still cannot refuse a global device whose interrupt cannot be aimed at the CPU: the clause its comment describes waits for a real `set_affinity` (interrupt-system P4).

**How it was found**
By the independent review of the design (`.agents/tasks/irq-multi-arch/DESIGN.md`, 2026-11-01): its reader of this tree noticed that the refutation of the PLIC storm held for the attach with a mode and not for the two-argument attach, which still let a Oneshot action onto a chip with no `trigger_mode`.

**The user's prediction**
None. The user approved the step, both the stubs and the refusal, on 2026-11-01 ("可以"), which decides D8.

**Three questions you should be able to answer**
- Why does a Oneshot action on the edge flow not hold its line, and why is that right for an edge pin and wrong for a pin the chip cannot read?
- Why does attaching with a mode make the same action safe on a chip that cannot say its mode?
- Why is `issue_eoi()` never deferred to `finalize_oneshot()`, and what would a PLIC do if it were?

## A controller with no mode register leaves `trigger_mode()` null, and the contract's other loose ends (2026-11-01)

**Files**
- `kernel/include/ours/irq/chip.hpp` — the contract rewritten in four places; `kernel/include/ours/irq/mod.hpp`, `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp` — the comments of the refusal.
- `kernel/commands/irq_chip_test.cpp` — the header and a comment say what the chip stands for; the naming suppression, which suppressed nothing, is gone.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md` — the future tenses and cited lines the previous entry left behind.

**Symptom**
Latent. The previous entry's contract told a controller with no mode register to record the mode it was told and answer it from `trigger_mode()`. A chip that does so has a `trigger_mode()`, and the refusal it added to the attach with no mode fires only for a chip that has none. The record called the PLIC the case the refusal matters for, and the test stood a chip with no `trigger_mode()` in for it, which is the shape the contract told a PLIC author not to write. A PLIC chip following the text would never meet the refusal.

**Cause**
Two halves of one change written from two directions. The refusal came from the review of the design (the two-argument attach keeps whatever flow the line has, and a chip that cannot say its mode gave it the edge flow); the contract's sentence about recording came from the earlier worry that `trigger_mode()` "must answer something". Neither needed the other: the layer already chooses the flow from the mode an owner tells it (`configure()`, or an attach with a mode), so the chip need not remember the mode, and a chip that answers a mode it was never given (a zeroed table answers Edge) would give a level source the wrong flow. The independent review of step 3 found that the two disagreed.

**Change**
The contract now says a controller with no register for the mode accepts the mode it is told, which is the firmware's and which it cannot check, and leaves `trigger_mode()` null: its lines get the edge flow until an owner gives them a mode; the attach that reads the mode and the attach with no mode for a Oneshot action answer NotSupported. No code changed in the layer: the refusal and the test were already right. The other loose ends of the review, all in comments and documents: `trigger_mode()` is called with L1 as well (the bind and the attach that reads the mode) and without it, so no operation takes a line lock; "none waits for another CPU" means for another CPU to act, since the chip's own lock is taken; `issue_eoi()` is called on a source `mask()` has masked, which the flows do for a line nobody is on and for a Oneshot interrupt held for its owner, so a controller that ignores the end of interrupt of a masked source masks another way (a PLIC: the priority register); a refused `configure()` leaves the source as it was; `startup()`, `issue_ack()` and `dump()` are not called and `platform_data` is null; the hardware numbers of a machine are one numbering, disjoint for each instance, because one table and one chip serve them all; a null `unmask()` also refuses `irq::unmask()`, which every owner calls, so such a chip serves no owner until the flag exists; the refusal looks at what the chip can say and not at the flow the line has now; `irq::attach()` documents the new NotSupported and that it comes before AlreadyBound; and `Chip::mask`'s "attach() with no mode" now says "the attach that reads the mode", since the word meant two overloads.

**Reference**
The step 3 entry says "None, this project's own design", which is not quite so: the contract's clauses come from how the line layer is modelled on Linux's genirq (`handle_fasteoi_irq`, `irq_chip`'s `irq_mask`, `irq_unmask`, `irq_eoi`, `irq_set_type`), unchecked here, and from the claim and complete of the PLIC and the acknowledge and end of the GIC read by a review agent on 2026-11-01. In Linux a chip whose `irq_set_type` accepts without effect is the PLIC's case (`irq-sifive-plic.c`, from the same reading).

**Verification**
- `boot_check.py`: dbg PASS, 44 passed and 4 skipped; rel PASS, 41 passed and 8 skipped, 6 of 6 required; host tests 154 passed. `irqchip.test` 18 checks and the other five kernel-shell tests unchanged and ok after the comment changes.
- Not verified: a PLIC chip written to the new text, which does not exist; that is the riscv branch's.

**Deliberately not done**
No per-source table in the chip, and no flag for the deferred mask. The PLIC-specific advice (mask with the priority register, keep the enable bits set) stays in `.agents/tasks/irq-multi-arch/DESIGN.md` for the PLIC chip's author and is not in `chip.hpp`, whose clause is the generic one.

**How it was found**
By the independent review of d964e083 (three reviewers, 2026-11-01): one of them read the contract's sentence about recording against the refusal and saw that a chip following the text never reaches it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a chip that answers `trigger_mode()` from a table nobody filled give a level source the wrong flow?
- Where does a line with a null `trigger_mode()` get its flow when an owner attaches with a mode?
- Why is `issue_eoi()` called on a masked source, and what does a PLIC do with the completion of a source whose enable bit is clear?

## `Domain::init` takes a floor for the first virq, so two domains can be given disjoint numbers (2026-11-01)

**Files**
- `kernel/include/ours/irq/domain.hpp`, `kernel/kernel/irq/domain.cpp` — `init(hirqnums, min_first = 0)`, and the paragraph of the header that says where a domain's virqs start.
- `kernel/commands/irq_domain_test.cpp` — `irqdomain.test` gains the floor cases and the refusals at the end of the number space (87 checks, 39 before).
- `.agents/tasks/irq-multi-arch/PLAN.md` — step 6 marked done.

**Symptom**
Latent. Nothing on x86 changes: the machine has one domain. `Domain::init` started each domain at the first multiple of 32 past its own highest hardware number, so a second domain built the same way would start at the same virq as the first and give two lines one number. An action records its line by virq alone, so the two lines could not be told apart.

**Cause**
Not a mistake: the rule was written for one domain per machine ("every virq is past all of them", `domain.hpp`), and D10 (b) (a table per controller, decided 2026-11-01 against the recommendation) makes a second domain possible. The start of a domain was a function of its own list only, with no way for the caller to say which numbers were already taken.

**Change**
One defaulted parameter, the lowest virq the domain may start at, and the start is the higher of the two bounds, rounded up to a multiple of 32:

```cpp
auto const first =
    ustl::round_up(std::max(nr_hwirqs, static_cast<usize>(min_first)), VIRQ_ALIGN);
```

The floor is widened to `usize` before it is rounded, so a floor near the end of the 32-bit space rounds up to 2^32 and is refused by the check that was already there (`first + count >= INVALID_V_IRQ_NUM`, InvArgs), and does not wrap to a low number. A floor of 0, the default, is no floor, and `DescTable::init` still passes none, so the machine's numbers are the numbers it had. The header says what the floor is for: the caller gives the first virq not yet taken by another domain, and a virq of one domain may equal a hardware number of another, which the old text ruled out only because there was one domain. The allocator that works out the floor is the directory, a later step; this step is the piece of `Domain` it needs.

**Reference**
None, this project's own design: D14 of `.agents/tasks/irq-multi-arch/DESIGN.md` (a global allocator, one contiguous block per domain, blocks not reused) chose the allocator, and this is the half of it that lives in `Domain`.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before the change.
- `irqdomain.test`: 87 checks, all ok. A floor above the domain's own start, between two multiples of 32, one past a multiple of 32, at the start, below it, 0, and above a start a high hardware number already raised; refused: a floor at `INVALID_V_IRQ_NUM`, one that rounds out of the space, a list of 32 lines from the last multiple of 32 with room; the last multiple of 32 with room for one line is taken, and a refusal keeps nothing. The other five kernel-shell tests unchanged and ok: `irqchip.test` 18, `irq.test` 72, `interruptline.test` 63, `pinlines.test` 28, `interrupt.test` 58 (`pinlines.test` and `interruptline.test` unchanged is what shows the machine's numbers did not move).
- clang-tidy on the two sources and the test: no finding on a changed line (one on the test, a local named in capitals, fixed).
- Four mutations, each restored byte for byte and rebuilt, each killed by `irqdomain.test`: the floor ignored; the floor not rounded; `min` for `max`; the floor cut to 8 bits.
- Not verified: a second domain on a real machine, or any caller that passes a floor; there is none yet.

**Deliberately not done**
- No accessor for the end of a domain's virqs and no allocator: the directory is step 8.
- `DescTable::init` does not take a floor; it passes none, and the facade takes one when the directory exists.

**How it was found**
By the independent read of the tree for D10 (b), 2026-11-01 (`.agents/tasks/irq-multi-arch/PLAN.md`, steps 6 to 10): `domain.cpp:26` starts each domain from its own list, so two would overlap.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the floor widened to `usize` before it is rounded up, and what would a floor of `INVALID_V_IRQ_NUM - 30` do if it were not?
- Why may a virq of one domain equal a hardware number of another, when none may equal a hardware number of its own?
- Why does the default of 0 keep the machine's numbers exactly, and which test shows it?

## The kernel can take a line away from its actions: a cancel hook, a Dying mark, `revoke()`, and a destroy that always waits for the line (2026-11-01)

**Files**
- `kernel/include/ours/irq/{types,action,desc,desc_table,mod}.hpp`, `kernel/kernel/irq/{desc,desc_table,mod}.cpp` — `IrqCancel`, the action's optional hook and atomic line number, `Desc::check_revocable()`, `mark_dying()`, `unmark_dying()`, `revoke()`, the table's `revoke_line()` and `unmark_dying()`, and two test hooks in the facade.
- `kernel/object/interrupt_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_dispatcher.hpp` — `line_revoked()`, and `destroy()` waiting for the line also when the object is destroyed already.
- `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — the object's cancel hook, and the assertion that allows NotFound.
- `kernel/commands/irq_revoke_test.cpp` (new, `irqrevoke.test`), `kernel/commands/interrupt_line_test.cpp`, `kernel/commands/interrupt_test.cpp`, `kernel/commands/BUILD.gn` — the tests.
- `system/public/ours/syscalls.gni` — `interrupt_create` says NoResource also for a line being taken away.
- `.agents/tasks/irq-multi-arch/PLAN.md` — step 9 marked done.

**Symptom**
None yet, nothing calls it. D16 lets a domain be removed after boot, and the removal has to take the actions off every line of the domain from the kernel's side. Today an action leaves its line only when its owner detaches it, and an interrupt object is freed on the strength of that: `destroy()` runs `irq::detach()`, which is a barrier, and only then may the object go. The kernel taking the action off instead breaks that chain, and the review of the design found a use after free in it.

**Cause**
Two parties could now end an action's life, and the first to finish had nothing that made the second wait. The removal unlinks the action under the line's lock, tells the owner, and has one thing left to do to it, clearing its line number. The owner's `destroy()` of an object already destroyed (as the removal's hook leaves it) returned at once, before the line, so the last handle closing on another CPU could free the object, with the action embedded in it, while the removal was about to write into it.

**Change**
- An `Action` may carry a cancel hook (`IrqCancel`). Its line number is atomic: written under the line's lock, read by `detach()` without it, with acquire.
- `Desc::mark_dying()` refuses a Permanent line and a line with an action that gave no hook (AlreadyBound) and otherwise marks the line; from then on `attach()` (both forms), `unmask()` and `configure()` answer NoResource, the check before the chip is touched, and a finalize does nothing. `check_revocable()` is the same refusal without the mark, and `unmark_dying()` undoes the mark.
- `Desc::revoke()`, under the line's lock with preemption off (a hook may wake a thread): for each action, unlink it, run its hook, and only then store INVALID into its line number with release, which is the removal's last touch of the action. An owner's `detach()` either reads the line valid and queues on the lock behind the removal, and then finds the action gone (NotFound), or reads INVALID, and so knows the removal is done with the action and with the object it is embedded in. Held goes with the actions, and the line is masked last.
- `InterruptDispatcher::destroy()` runs `unregister_interrupt_handler()` also when the object is destroyed already, so every destroy waits for the line. The object's hook, `line_revoked()`, is `destroy()` without that detach: the state becomes Destroyed, a parked waiter wakes (Cancelled), a bound port loses the queued packet. The assertion in the object's detach allows NotFound.

The order inside `revoke()` (hook first, line number last) is what a mutant that swaps the two fails on, deterministically: the hook asks whether the action is still on its line as its owner reads it.

**Reference**
None, this project's own design (D19 of `.agents/tasks/irq-multi-arch/DESIGN.md`). The refusal of a line with an action that did not agree to be taken off is the opt-in the design review asked for.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before.
- `irqrevoke.test` (new) on four CPUs, 29 checks, all ok: `revoke()` tells two shared actions once each with the line's number, while each is still on that line as its owner reads it, takes them off (`detach()` then answers NotFound), masks the line once, and nothing handles an interrupt on it; a marked line refuses `attach()` with a mode or without, `unmask()` and `configure()` with NoResource without the chip being asked, still masks, and takes actions again once unmarked; a Permanent line and a line with an action without a hook refuse the mark and are left as they were, with no mark left behind; with a handler in flight for 20 ms on another CPU, `revoke()` returns after it and the hook is told after it; and 400 rounds of an owner's `detach()` against the removal, each side held back by turns (the detach won 199, the removal 201), where no detach that answered NotFound did so before the hook was done.
- `interruptline.test` 78 checks (63 before): a parked waiter wakes with Cancelled, a later wait answers Cancelled, the marked line refuses an object (NoResource), the object's own destroy answers Ok twice, the line takes an object again once unmarked and closing the revoked object's handle leaves the new owner the line; bound to a port, the queued packet is taken back, an ack answers Cancelled, and the line is free after. `interrupt.test` 64 checks (58 before): a test-local object that counts its detaches shows every destroy detaches, also one that finds the object destroyed already, and that a revoke detaches nothing. The other tests of the layer unchanged and ok: `irqrcu.test` 34, `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `pinlines.test` 28.
- clang-tidy on the changed sources and the three tests: no finding; the formats are clean.
- Ten mutations, each restored byte for byte, each killed (failed checks in brackets): the line cleared before the hook (1 of 29); a second destroy not waiting (2 of 64); unmask ignoring the mark (2 of 29); attach ignoring it (2 of 29); a hookless action counted revocable (1 of 29; the first form did not compile, an unused variable, and was rerun as `&& false`); revoke not masking the line (1 of 29); a Permanent line revocable (1 of 29); revoke leaving the action on its line (2 of 29); the revoke not waking the waiter (13 of 54, the test then waiting out its time limits); the revoke leaving the packet queued (1 of 78).
- Not verified: the use after free itself with the destroy's wait removed, other than by counting the waits and by the race above, whose window the test cannot widen; `revoke()` on a Oneshot level line that is held (the held bit is cleared, argued, not tested); weak-memory machines; more than four CPUs; the release image's behaviour of the new DEBUG_ASSERTs.

**Deliberately not done**
- Nothing calls `revoke_line()` but the tests, through `irq::revoke_line_for_test()`: the directory and `unregister_domain` are step 10, and the facade step 11.
- No notice to a driver whose interrupt object is bound to a port: it gets none (D20), only an Ack or bind that answers Cancelled.
- No test of the use after free itself when the destroy's wait is removed, other than by counting the waits (`RecordingInterrupt`): the interleaving needs the removal paused between its hook and its last store, which nothing in the test can do.

**How it was found**
By the independent review of the design of D16, 2026-11-01: two reviewers found the use after free between the hook and the owner's destroy.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `revoke()` store INVALID after the hook and not before, and what can the owner's `detach()` conclude from reading INVALID?
- Why must a `destroy()` of an object already destroyed still wait for the line?
- Why does `mark_dying()` look at the actions' hooks under the line's lock, and what would `check_revocable()` followed by `mark_dying()` without that do?

## A finalize on a marked line is not lost, and the tests of the removal catch what the first ones missed (2026-11-01)

**Files**
- `kernel/kernel/irq/desc.cpp`, `kernel/include/ours/irq/{desc,mod}.hpp` — `finalize_oneshot()`, `revoke()`'s checks, and the docs of the answers.
- `kernel/object/interrupt_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_event_dispatcher.cpp` — a DEBUG_ASSERT and comments.
- `kernel/commands/irq_revoke_test.cpp` (rewritten), `kernel/commands/interrupt_test.cpp`, `kernel/commands/interrupt_line_test.cpp`.
- `system/public/ours/syscalls.gni`, `.agents/skills/ours-kernel-design/references/task.md`, `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md`.

**Symptom**
Latent, found by the independent review of the previous entry. A removal that marked a Oneshot level line and was then refused, because a later line of the domain would not be marked, left the line held and masked: while the mark stood, the owner's ack ran `finalize_oneshot()`, which returned at once on a marked line, and the mark was then taken off without touching the hold. The interrupt object was already idle, so nothing would raise another interrupt, and every later wait parked for good. The previous entry's claim that a refused removal "changes nothing but a short window" was false for this call.

**Cause**
`finalize_oneshot()` returned early on a marked line (`|| is_dying_locked()`), copied from the three calls that start or reconfigure a line, which should refuse one. A finalize starts nothing: it ends a debt the owner has paid, and a removal that goes through clears the debt itself and masks the line last, under the same lock, so skipping it protected nothing and could lose the ack.

**Change**
- The clause is gone: a finalize on a marked line unmasks it, as on any other. `revoke()` checks its two preconditions (the line is marked, not Permanent) and that each action has a hook with `ASSERT`, which holds in the release image, the path being cold.
- `line_revoked()` asserts the object is not virtual. Comments say what became false or incomplete: the object's failed-unmask path (a mark can land between its attach and its unmask), the finalize that a removal also skips, `destroy()` answering Ok after a revoke where it would have answered NotFound, the facade's lists of answers (NoResource also for a line being taken away; detach's NotFound also for an action the kernel took off, which means the removal is done with it), and `desc.hpp`'s steps, now numbered 0 to 2.
- `syscalls.gni`: `interrupt_destroy` says the kernel taking a line away destroys the object too. `task.md` has one bullet for the cancel hook, the mark and the destroy that always waits.

The tests were weaker than the record said, and now are not:
- The race goes through a line table of the test's own, so a detach that finds the line number cleared answers without the line's lock, which is the path the design leans on, and the hook is slow (600 spins) so the detach lands during it; one side is held back by up to 400 spins by turns. The earlier version detached through a line that took the lock first, so a detach that answered NotFound found the hook done by the lock's hand-off alone.
- A hook is told the line's virq, not the pin's number (the test's lines have two different numbers), and runs with interrupts and preemption off, as it is promised.
- A line that was Shared takes an exclusive action again and then refuses a shared one; a held Oneshot level line is not held by the next owner; a finalize on a marked line unmasks, and the line is not held once unmarked.
- The barrier case no longer depends on the command thread: the handler holds its CPU until the revoke is announced and then for 20 ms, the command thread is on another CPU, and the check is that the handler was still running when `revoke()` was called and had returned when it came back.
- In `interrupt.test`, a destroy on an object whose line is being taken away, with the removal held in the object's hook, does not return until the removal lets go (three CPUs). That is the use after free itself, held still, where the previous entry counted waits.
- `interruptline.test` unmarks the real table's free line on the one failure path that returned without it.

**Reference**
None, this project's own design.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before.
- `irqrevoke.test` 37 checks (29 before), `interrupt.test` 67 (64), `interruptline.test` 78 unchanged in count; the other tests of the layer unchanged and ok: `irqrcu.test` 34, `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `pinlines.test` 28. The race ran 400 rounds on four CPUs and the detach won 201 and the removal 199, with the hook slow enough that the detach lands during it.
- clang-tidy on the changed sources and the three tests: no finding after two constexpr locals were renamed; the formats are clean.
- Six mutations, each restored byte for byte, each killed (failed checks in brackets): a finalize skipping a marked line (2 of 37); revoke leaving Held, Shared and Oneshot (2 of 37); revoke without preemption disabled (1 of 37); the line cleared before the hook (3 of 37 and 2 of 67: the hook's own check, the race through the table, and the held removal all see it); a second destroy not waiting (5 of 67); configure ignoring the mark (2 of 37). The first four of these and the destroy one are the mutants the review said survived or were not listed.
- Not verified: the Dying checks in `finalize_oneshot()` are gone, so no mutant there is left to run; the hook told the pin's number instead of the virq (a mutation in `Action::cancel()` would rebuild the whole tree; the test checks it by value); weak-memory machines; more than four CPUs; the chain case with fewer than three CPUs (it is skipped and says so).

**Deliberately not done**
- `DescTable::revoke_line()` marks and revokes in one call, for the tests and a domain of one line; the loops over a domain's lines (the read-only pass over every line, the marks with their rollback, then the revokes) are the directory's, step 10, which will call `check_revocable()`, `mark_dying()`, `unmark_dying()` and `revoke()` on its own lines. The poisoning of the action's memory after a detach, which the plan names, was replaced by the checks of the hook's last act, and by holding the removal still in the hook.
- Unrelated Dying checks stay as argued: `mask()` works on a marked line on purpose.

**How it was found**
By the independent review of `a216cbcf` (three reviewers, 2026-11-01): the concurrency reviewer found the lost finalize; the test reviewer that the race could not fail, that the barrier could pass by luck of placement, and which mutants survived; the conventions reviewer the false and missing comments.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a finalize on a marked line have to work, when attach, unmask and configure on it refuse?
- Why can the earlier race test not fail for the property it names, and what did making the hook slow and the detach go through the table change?
- Why must the command thread of the barrier case not share a CPU with the handler, and what does the handler wait for?

## `irq::DomainRegistry`: a machine's interrupt domains, each with its table and a block of virqs, registered and removed after boot (2026-11-01)

**Files**
- `kernel/include/ours/irq/domain_registry.hpp`, `kernel/kernel/irq/domain_registry.cpp` — new: `DomainId`, `DomainSlot`, `DomainRegistry` with `register_domain()`, `unregister_domain()`, `table_of()`, `find()` and `virq_of()`; `kernel/kernel/irq/BUILD.gn` lists the source.
- `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp` — `init()` takes the floor for the virqs, the table reports its block, and it has `begin_removal()`, `abort_removal()`, `finish_removal()` and `destroy()`.
- `kernel/include/ours/irq/domain.hpp` — `first_virq()` and `num_virqs()`.
- `kernel/commands/irq_registry_test.cpp` (new, `irqregistry.test`), `kernel/commands/BUILD.gn`.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md` — step 10 marked done.

**Symptom**
None, nothing uses it yet: the line layer still has the one table of the machine. D10 (b) of `.agents/tasks/irq-multi-arch/DESIGN.md` gives every interrupt controller a table of its own, so a control call, which has only a virq, has to find the table it belongs to, and interrupt_map, which will have a domain and a hardware number, the virq; and D16 lets a domain be removed after boot.

**Cause**
A missing piece (D10 (b), D12 to D17), built unwired on purpose: the facade, the root handler and interrupt_map are steps 11 and 12.

**Change**
The registry holds at most `CAPACITY` (8) domains. Each has an id, a table (`DescTable`), a block of virqs [first, end), and the registrant's `DomainSlot`. The readers (`table_of()`, `find()`, `virq_of()`) take no lock: they are `task::IrqRcuReadGuard` sections over an array of `IrqRcuPointer<Entry>`, one place per domain, and what they return is good inside the section. The writers take one leaf spin lock for a few words, never while allocating.

- `register_domain()` reads the end of the last block given out under the lock (the floor), builds the table outside it with that floor (`DescTable::init(..., floor)`, which passes it to `Domain::init()`, step 6), then takes the lock again, re-checks the id and the floor (another registration may have moved either; then the table is thrown away and built again) and publishes the entry and the slot with release stores. A table that was never published is freed at once.
- `unregister_domain()` claims the domain as Removing (a second remover gets BadState, and the id stays taken), `begin_removal()` marks every line or none (a read-only `check_revocable()` over every line first; `mark_dying()` over every line, undoing the marks if one refuses), `finish_removal()` revokes every line, then the slot and the registry's pointer are retracted, `irq_rcu_synchronize()` waits for the readers, and only then are the table (`destroy()`: lines poisoned 0xDD, then freed), the entry and the place freed.
- Root is not removable (NotSupported). The mark of virqs given out is never lowered, so a removed domain's block stays dead and its id, if registered again, gets a block above it.

The design said a linked list of entries; this is an array of pointers, one per place. A refused or raced registration then has nothing a reader could be passing through, and frees at once.

**Reference**
None, this project's own design: D12 (the registrant chooses the id), D13, D14 (blocks from a global mark), D16 and the "Reclaiming a removed domain" section of `DESIGN.md`. The array of published pointers, scanned by a reader, is the shape of Linux's RCU-protected lookup tables (from memory, not re-read).

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before.
- `irqregistry.test` (new) on four CPUs, 41 checks, all ok: two domains over the same hardware numbers hold the blocks [32, 36) and [64, 68), the first the numbers a lone domain gets; a virq finds its domain's table at both ends of its block, and the hole between the blocks, past them, 0 and the invalid number find none; the lookup by domain and pin; the slot holds the table; registration refuses the Invalid id, an empty list, a number that is no hardware number, a chip that cannot end an interrupt, a number given twice, an id in use and a ninth domain, keeping nothing; removal answers NotFound for an unknown id and NotSupported for Root, a removed domain's lookups and slot find nothing and its id is registered again in a block above everything given out; a domain with a Permanent action, and one with an action with no hook, are not removed (AlreadyBound), keep working and tell nothing, and the second is removed once the hookless action is gone, its hooked action told once; and 200 domains registered and removed with three readers on other CPUs looking at the table (594 to 597 sightings, none broken).
- The other tests of the layer unchanged and ok: `irqrevoke.test` 37, `irqrcu.test` 34, `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `interruptline.test` 78, `pinlines.test` 28, `interrupt.test` 67.
- clang-tidy on the new and changed sources and the test: no finding; the formats are clean.
- Six mutations, each restored byte for byte, each killed: no grace period (1 of 41 checks fail: the stress); the registry's pointer not retracted and the slot not retracted (killed, but the run did not finish: the table was freed while still published, so no count); the floor not passed (4 of 41); Root removable (3 of 41); the mark of virqs given out not moved (4 of 41).
- Not verified: the rollback of `begin_removal()`; a registration racing another (the retry); the Dying marks left by a removal refused after the first pass; more than four CPUs; weak-memory machines; the use of the registry from an interrupt handler (nothing does yet).

**Deliberately not done**
- Nothing registers a domain but the test: the facade is on the one table of the machine until step 11, and `interrupt_map` still takes one number until step 12.
- The precondition against a boot on which an application processor gave up is not tested for: after the second review of `irq_rcu_synchronize()` the fence and the sequentially consistent join are what carry that argument, and the call reads the online mask again.
- Domains are named `DomainRegistry` and not the "directory" of the design documents; the documents keep the older word (the user left the naming for later).
- The rollback of `begin_removal()` (a mark that refuses after the read-only pass accepted the line) is argued, not tested: only a hookless action attaching between the two passes reaches it.

**How it was found**
By the plan, `.agents/tasks/irq-multi-arch/PLAN.md` step 10, after the design of step 7.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a registration build its table outside the lock and then check the floor again, and what happens to the table if the floor moved?
- Why does `unregister_domain()` retract the registry's pointer and the slot before the grace period, and free after, and what would a reader that began in between find?
- Why is the block of a removed domain never given out again, when its id may be?

## The registry's writers never read an entry, and the tests of the registry catch what the first ones missed (2026-11-01)

**Files**
- `kernel/kernel/irq/domain_registry.cpp`, `kernel/include/ours/irq/domain_registry.hpp` — the writers' scans, the asserts, the slot check, the lock annotations.
- `kernel/kernel/irq/desc_table.cpp`, `kernel/include/ours/irq/{desc_table,desc,domain}.hpp`, `kernel/kernel/irq/desc.cpp` — `destroy()`, `has_actions()`, and comments.
- `kernel/commands/irq_registry_test.cpp` — more cases.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md`.

**Symptom**
A use after free on the heap, reachable by two threads, found by the independent review of the previous entry. `unregister_domain()` freed the removed domain's `Entry` while its place was still Removing and still pointed at it, and gave the place back in a second hold of the lock, with the thread preemptible in between. Every other writer scans the places and read `m_entry[i]->id` of each taken place. A registration of another id could take the freed block for its own new entry and then find its own id on the stale place (AlreadyExists for an id nobody holds, its fresh table thrown away); a removal of an id registered at a later place could meet the stale place first and answer BadState for a domain that was there. Nothing in the tests had two writers.

**Cause**
The ids were read through the entries, which only the remover and the readers may touch: the entry is freed at the end of a removal, while the place that points at it is still taken. The id of a place is the writers' own fact and belongs with their state, under their lock.

**Change**
- `m_id[CAPACITY]` is kept beside `m_state[]`, under the lock, and the writers compare ids there. `m_entry[]` is read by the remover that claimed the place and nobody else. A place is given back with its entry cleared first, and the entry freed after.
- The context asserts of a removal (interrupts on, preemption on, the hotplug lock not held) are at the top of `unregister_domain()`, before the lines are revoked, and in the release image: after the revoke the removal cannot be undone, and a call from where it may not wait would have been trapped by `irq_rcu_synchronize()` only then.
- A slot that already holds a table is refused (BadState) under the lock in every build; it was a debug assertion after the table was built, and a second domain on the slot would have lost the first one's table at its removal.
- `DescTable::destroy()` asserts every line is empty, and writes the poison through a volatile pointer, which the compiler may not drop as a store to memory about to be freed; the comments no longer say a reader finds a canary gone, which nothing checks. `abort_removal()` is gone (no caller); `begin_removal()` says it takes no line to be marked already. The lock-guarded members are annotated (`TA_GUARDED`, `TA_REQUIRES`).

The tests grew from 41 to 66 checks: a sparse domain removed and others registered after it; an action on each of four lines, the last without a hook, which refuses all four, and without it every other action told once; one id registered and removed three times as often as the registry has places, each time in a higher block; a slot that holds a table refused; a domain being removed, held in the grace period by a reader on another CPU, is no longer found, cannot be registered again (AlreadyExists) or removed again (BadState), and its id returns when the reader leaves; two writers registering and removing ids of their own at once, none refused for an id nobody holds and no two blocks given out overlapping (the case that would have found the use after free, and that exercises the retry of a raced floor); and the stress readers now also read the slot a root handler holds, and count the looks the removal came under: 598 of 604.

Corrections to the previous entry: its Change said a re-check may find the id taken *or* the floor moved, and then the table is built again; only a moved floor retries, a taken id is refused. Its Reference named a "shape of Linux's RCU-protected lookup tables" without a function: there is none claimed now, and the Reference stays "None, this project's own design". Its Files line should have listed `DESIGN.md` as edited for the test lists only; the design documents did not say the directory is an array of eight published pointers, the code's name for it, or that two things the design asked for were not built (see below). The blank line before its "Deliberately not done" is added.

**Reference**
None, this project's own design.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before.
- `irqregistry.test` 66 checks (41 before), all ok on four CPUs: 200 domains registered and removed with three readers, 600 looks, the removal came under 598 of them, none broken; the other tests of the layer unchanged and ok: `irqrevoke.test` 37, `irqrcu.test` 34, `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `interruptline.test` 78, `pinlines.test` 28, `interrupt.test` 67.
- clang-tidy on the registry, the table, the line and the test: no finding; the formats are clean.
- Five mutations, each restored byte for byte, each killed: a place being removed counted as free; the raced floor not retried (1 of 66, the two writers); a slot that holds a table accepted; `finish_removal()` stopping after the first line (the empty-line assertion of `destroy()` traps); no grace period (4 of 66). Three of them (the first, the slot, the first line) ended the run in an assertion rather than with a count of failed checks, so there is no count for them.
- Not verified: the use after free of the first version itself is no longer in the code, so it has no mutant (two writers at once passes either way unless the heap reuses the block in the window, which is what the review reasoned from); the rollback of `begin_removal()` after a mark that refuses (only an action attaching between the two passes reaches it); the registry on fewer than three CPUs (the cases that need them are skipped and say so); more than four CPUs; weak-memory machines.

**Deliberately not done**
- The design asked for a fake chip whose operations assert the table is alive, and for a CPU taken offline and brought back while a domain is removed. Neither is built: the stress readers check the table's block and its destroyed flag instead, which a freed table fails whether or not a chip is called, and the registry has no use of the hotplug state that `irq_rcu_synchronize()` does not already have.
- The precondition that a removal is refused on a boot on which an application processor gave up was dropped at step 8, not left undone: after the review of `irq_rcu_synchronize()` the fence and the sequentially consistent join carry that argument, and the call reads the online mask again. `DESIGN.md` D19 now says so.
- The capacity of eight domains, and a scan of the places on every control call, are put to the user with step 11, which wires them to the control calls.

**How it was found**
By the independent review of `515af6b6` (three reviewers, 2026-11-01): the concurrency reviewer found the use after free by reading the writers' scans against the end of a removal; the test reviewer that no test had two writers or saw a domain being removed; the conventions reviewer the design and record drift.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why may a writer not read an `Entry`, when a reader may, and who else may?
- Why can the case with a reader holding the grace period tell a place being removed from one that is gone, and what would a registration of the same id do if it could not?
- Why are the blocks the two writers are given never overlapping, and which part of `register_domain()` does that rest on?
