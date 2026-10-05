# kernel-irq outstanding repairs

## 1. When two ISA lines land on one GSI, the lower ISA number keeps it, where Linux gives it to the line an override moved there

**Severity**
Medium: the wrong ISA line would own the GSI, but only on firmware whose MADT moves one ISA line onto another's own pin, other than ISA 0 onto GSI 2. QEMU's does not.

**Symptom**
Latent, cannot be triggered today. On such firmware, say an override of ISA 9 onto GSI 5 with ISA 5 left alone, the boot logs `ISA IRQ 9 shares GSI 5 with an earlier ISA line; not programmed`, the IO-APIC entry for GSI 5 carries ISA 5's trigger mode and polarity instead of the override's, and a driver for the device the firmware actually wired to GSI 5 gets a line configured for the wrong one.

**Cause**
`platform_init_apic()` (`kernel/platform/pc/interrupt.cpp`) walks the ISA lines in order, 0 to 15, and gives each GSI to the first line that reaches it, skipping any later line on the same GSI so that `irq::init_lines()` is not handed a number twice. The order says nothing about which line is real. An override is the firmware saying where a line is wired; a line with no override only defaults to the pin of its own number. When the two meet on one pin, the override is the one to believe. The ISA 0 and 2 pair, the only one QEMU has, comes out right only because ISA 2 is skipped before the check, as Zircon skips it.

**Change**
Program the lines an override moved, those whose GSI differs from their ISA number, in a first pass, and the rest in a second pass that skips a GSI already taken, so an override always wins. Rejected: programming every line and letting the later one overwrite the entry, as Zircon's `platform_init_apic()` does, since the table would then hold a line whose entry belongs to another, and `init_lines()` refuses a repeated number.

**Reference**
Linux `mp_config_acpi_legacy_irqs()` (`arch/x86/kernel/acpi/boot.c:1113-1177`), which runs after the overrides are recorded and skips an ISA IRQ whose IO-APIC pin already has a mapping. Zircon `platform_init_apic()` (`zircon/kernel/platform/pc/interrupts.rs:109-206`) has no such check.

**Acceptance**
`python3 .agents/skills/ours-verify/scripts/boot_check.py` passes, and with a fake override of ISA 9 onto GSI 5 injected by hand into the parsed list in `platform_init_apic()`, dev.dbg_1 logs that ISA 5, not ISA 9, was not programmed, and the IO-APIC trace line for GSI 5 shows the override's trigger mode.
