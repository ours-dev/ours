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
