# x86-apic outstanding repairs

## 3. `XAPIC_CHIP`'s mask and unmask always touch LINT0

**Severity**
Medium: wrong, but not reached today. `XAPIC_CHIP` is `[[maybe_unused]]` and bound to no line (`kernel/arch/x86/apic/local_apic.cpp:111-119`).

**Symptom**
Latent, cannot be triggered today. Once the chip is bound to a line, masking or unmasking any local interrupt masks or unmasks LINT0 instead, and leaves the intended LVT entry as it was. LINT0 is the pin firmware may wire to the legacy PIC, so an unmask meant for something else could let PIC interrupts through.

**Cause**
Both lambdas ignore their `irq::Data` and name `arch::XApicRegType::LvtLogicalInt0` (`local_apic.cpp:113-116`). A chip operation has to act on the LVT entry of the line it is given, and nothing maps a line to its LVT register.

**Change**
Either:
- give the local interrupts numbers, and have the chip map `data.hirqnum` to its LVT register (timer, LINT0, LINT1, error, performance counter, thermal). Since irq-genirq step 3 (2026-10-05) a line's flow ends each interrupt through the chip's `issue_eoi`, and `init_lines()` refuses a chip without one; `XAPIC_CHIP` has its local APIC EOI under `issue_ack`, so binding it also moves that EOI to `issue_eoi`; or
- do as Zircon does: keep the LAPIC's local interrupts on fixed vectors outside the generic irq layer, as ours already does (`kernel/arch/x86/kernel/faults.cpp:250-286`), mask each through a function of its own, and retire `XAPIC_CHIP`.

The second removes a chip, which may be scaffolding for planned work, so ask the user before deleting it (`ours-kernel-design`: scaffolding is not deleted as cleanup).

**Reference**
Zircon masks each LVT entry through its own function, `apic_timer_mask()` and its siblings (`zircon/kernel/arch/x86/src/lapic.rs:531`, declared in `zircon/kernel/arch/x86/include/arch/x86/apic.h:73`), and its LAPIC vectors never pass through the generic interrupt layer (`zircon/kernel/arch/x86/exceptions_c.cc:503-542`).

**Acceptance**
No chip operation names an LVT register other than the one for the line it is given (or `XAPIC_CHIP` is gone, with the user's approval); `ours-verify` passes on both images.
