# x86-apic outstanding repairs

## 1. An IOAPIC older than version 0x20 never gets an EOI for a level-triggered pin

**Severity**
Medium: wrong, but only on an IOAPIC without an EOI register. The QEMU this project boots (`prebuilt/qemu`, `-M q35`) emulates version 0x20 by default, which has one.

**Symptom**
Latent, cannot be triggered today. On an IOAPIC whose version is below 0x20, a level-triggered interrupt routed through it fires once and then never again: the pin's Remote IRR bit stays set, and the IOAPIC delivers nothing more on that pin.

**Cause**
`ioapic_issue_eoi()` (`kernel/arch/x86/apic/io_apic.cpp`) calls `IoApic::issue_eoi()`, which answers NotSupported when the IOAPIC has no EOI register (version below 0x20) and writes nothing. The caller drops that answer, so on such an IOAPIC the EOI that clears Remote IRR is never sent. The drop is now explicit and points here (system-abi-surface step 2).

**Change**
On NotSupported, clear Remote IRR the old way, as Linux does: rewrite the redirection entry masked and edge-triggered, then write the original level-triggered entry back. `IoApic` already has `read_redirection()` and `write_redirection()` for it. The pin the EOI is for is known from `data.hirqnum`.

**Reference**
Linux's `__eoi_ioapic_pin()` (arch/x86/kernel/apic/io_apic.c): `io_apic_eoi()` when the version is at least 0x20, and otherwise the mask, edge, restore sequence.

**Acceptance**
A QEMU boot with an IOAPIC forced below version 0x20 (QEMU's `ioapic` device has a `version` property) still takes a level-triggered interrupt after its first, and `ours-verify` passes on both images.

## 2. `IoApic::issue_eoi` writes the GSI to the EOI register, where the vector belongs

**Severity**
Medium: wrong, but not reached today. Nothing calls the IO-APIC chip's `issue_eoi`: `IOAPIC_CHIP` is bound to no line (`kernel/arch/x86/apic/io_apic.cpp:92-102`), and every platform interrupt's EOI goes to the LAPIC (`kernel/arch/x86/kernel/faults.cpp:246-247`).

**Symptom**
Latent, cannot be triggered today. Once a level-triggered line's EOI goes through the IO-APIC chip, the IO-APIC clears Remote IRR on whichever redirection entry has the GSI as its vector, if any, not on the line's own entry. The line then takes one interrupt and never another. Under the static `32+GSI` vectors of `.agents/tasks/microkernel-interrupts/DESIGN.md` (D7), the value written is never the line's own vector.

**Cause**
`IoApic::issue_eoi(HIrqNum irq)` writes its argument to the EOI register (`kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp:137-143`), and `ioapic_issue_eoi()` passes it `data.hirqnum`, the GSI (`io_apic.cpp:69`). The IO-APIC compares the value written there with the vector field of each redirection entry, which is why Linux writes the vector. Item 1 is about IO-APICs that have no EOI register; this item is about what is written when there is one. They touch the same function but can be fixed separately.

**Change**
`IoApic::issue_eoi` takes an `arch::IrqVec`, so that a GSI no longer converts to it silently, and the chip passes the pin's vector, read from its redirection entry (`IoApicRouteEntry` needs a getter beside `set_vector`, `ioapic.hpp:15-17`). Deriving the vector as `32+GSI` was ruled out: it would tie the EOI to D7's static vectors, which are to become dynamic once PCI exists.

**Reference**
Linux's `io_apic_eoi(apic, vector)` and its caller `__eoi_ioapic_pin(apic, pin, vector)` (`arch/x86/kernel/apic/io_apic.c:264-268`, `:439-442`), which pass the redirection entry's vector (`:498`).

**Acceptance**
Passing a GSI to `IoApic::issue_eoi` no longer compiles; when the first level-triggered line goes through the chip (microkernel-interrupts step 11, or PCI INTx later), it takes more than one interrupt; `ours-verify` passes on both images.

## 3. `XAPIC_CHIP`'s mask and unmask always touch LINT0

**Severity**
Medium: wrong, but not reached today. `XAPIC_CHIP` is `[[maybe_unused]]` and bound to no line (`kernel/arch/x86/apic/local_apic.cpp:111-119`).

**Symptom**
Latent, cannot be triggered today. Once the chip is bound to a line, masking or unmasking any local interrupt masks or unmasks LINT0 instead, and leaves the intended LVT entry as it was. LINT0 is the pin firmware may wire to the legacy PIC, so an unmask meant for something else could let PIC interrupts through.

**Cause**
Both lambdas ignore their `irq::Data` and name `arch::XApicRegType::LvtLogicalInt0` (`local_apic.cpp:113-116`). A chip operation has to act on the LVT entry of the line it is given, and nothing maps a line to its LVT register.

**Change**
Either:
- give the local interrupts numbers, and have the chip map `data.hirqnum` to its LVT register (timer, LINT0, LINT1, error, performance counter, thermal); or
- do as Zircon does: keep the LAPIC's local interrupts on fixed vectors outside the generic irq layer, as ours already does (`kernel/arch/x86/kernel/faults.cpp:250-286`), mask each through a function of its own, and retire `XAPIC_CHIP`.

The second removes a chip, which may be scaffolding for planned work, so ask the user before deleting it (`ours-kernel-design`: scaffolding is not deleted as cleanup).

**Reference**
Zircon masks each LVT entry through its own function, `apic_timer_mask()` and its siblings (`zircon/kernel/arch/x86/src/lapic.rs:531`, declared in `zircon/kernel/arch/x86/include/arch/x86/apic.h:73`), and its LAPIC vectors never pass through the generic interrupt layer (`zircon/kernel/arch/x86/exceptions_c.cc:503-542`).

**Acceptance**
No chip operation names an LVT register other than the one for the line it is given (or `XAPIC_CHIP` is gone, with the user's approval); `ours-verify` passes on both images.
