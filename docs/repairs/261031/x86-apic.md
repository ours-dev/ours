# x86-apic outstanding repairs

## 1. A pin the firmware wired to NMI is overwritten by the platform's masked entry

**Severity**
Medium: not reached on QEMU, which has no such entry. Reached on hardware whose MADT has a non-maskable interrupt source entry for an IO-APIC pin and whose firmware programmed that pin for NMI delivery.

**Symptom**
Latent, cannot be triggered on the machines tested. On such hardware the pin's NMI delivery would stop after boot: the entry is rewritten masked, with a fixed vector.

**Cause**
Since the change "Every IO-APIC pin is programmed masked and has an interrupt line" (`docs/changes/261031/x86-apic.md`), `platform_init_apic()` programs every pin of every IO-APIC. Before it, only the pins the ISA lines reach were written and every other entry stayed as the firmware left it. Nothing reads the MADT's NMI source entries (type 0x03): `acpi::madt::NmiSourceEntry` (`kernel/lib/acpi/include/acpi/structures/madt.hpp`) is defined and is used only by the structure test, so the platform cannot tell which pin was meant for NMI.

**Change**
Read the NMI source entries in `platform_init_apic()` before the loop over the pins. For each, either leave that pin alone and give it no line, or program it for non-maskable delivery with the trigger mode and polarity the entry gives, as the MADT says; the first is the smaller change and keeps the kernel from owning a pin it does not use. Ignoring the entries, as now, is ruled out on hardware.

**Reference**
Not looked up. Whoever takes this should read how Linux and Zircon treat the entry (from memory, Linux's `acpi_parse_nmi_src()` in `arch/x86/kernel/acpi/boot.c` only prints it; unchecked).

**Acceptance**
A host test in `kernel/lib/acpi/test` that parses a MADT with an NMI source entry and yields its GSI, and a boot log line naming the pin that was left alone. QEMU cannot present such an entry without a custom ACPI table; the end-to-end check is on hardware that has one.

## 2. The GSI of a PCI function comes from a rule that holds for QEMU's q35 only

**Severity**
Medium: right on the machine the images run on, wrong on hardware, where a driver would take the wrong interrupt line and never get its interrupt.

**Symptom**
Latent on QEMU. On a machine whose chipset wires its PCI interrupts differently, or for a function behind a PCI bridge, the driver computes a GSI no pin of its device is on: its interrupt object waits for ever.

**Cause**
Nothing in the tree evaluates ACPI's `_PRT`, the table in which the firmware tells the operating system how (slot, pin) is wired to a GSI. The decision of 2026-10-31 (interrupt-system D9) was that the driver of P1 applies QEMU q35's rule: GSI = 20 + ((slot + pin) & 3) for a root-bus function at slot 0 to 24, 16 + pin for slots 25 to 29 and 31. The device's Interrupt Line register cannot be used instead: the virtio-blk reads 11 there, a legacy PIC number. The rule is in `.agents/tasks/driver-capabilities/RESULT.md`, not in the kernel; the kernel takes whatever GSI the caller of `interrupt_map` names.

**Change**
A user-space component that evaluates `_PRT` (an AML interpreter, so very likely a third-party library, which needs the user's approval) and answers `(bus, device, pin)` with `(GSI, trigger mode, polarity)`: step 8 of `.agents/tasks/interrupt-system/PLAN.md` (P2). The bus service then asks it, and calls `interrupt_map(root, Gsi, gsi)` with the answer. Ruled out: reading the table in the kernel (AML is policy, and a large interpreter in ring 0: microkernel-interrupts D4), and trying each pin until one fires (unreliable on a shared line).

**Reference**
None, this project's own design. QEMU's side: `hw/isa/lpc_ich9.c` (`ich9_cc_init()`, `ich9_pirq_to_gsi()`) and `hw/i386/acpi-build.c` (`build_q35_routing_table()`).

**Acceptance**
A boot where the driver gets its GSI from the table and not from the rule, on a machine whose routing differs from q35's (QEMU's `pc` machine routes PCI through the PIIX with a different rule), and receives its interrupt.
