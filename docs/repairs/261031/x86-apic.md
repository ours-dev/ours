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
