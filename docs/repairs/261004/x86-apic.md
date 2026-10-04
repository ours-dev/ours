# x86-apic outstanding repairs

## 1. An ISA override whose GSI no IO-APIC owns stops the boot

**Severity**
Low: firmware has to be wrong first, and QEMU's q35 cannot produce it. When it happens the machine does not boot at all, on both images.

**Symptom**
Latent, cannot be triggered today. If the MADT carries an interrupt source override for a valid ISA line (0 to 15) whose global system interrupt is outside every IO-APIC's range, `platform_init_apic()` (`kernel/platform/pc/interrupt.cpp`) programs that ISA line at boot, `apic_configure_isa_irq()` hands the override's GSI to `apic_configure_irq()`, `resolve_irqnum_to_ioapic()` finds no IO-APIC, and `ASSERT(ioapic)` panics (`kernel/arch/x86/apic/io_apic.cpp`). The ASSERT is not a DEBUG_ASSERT, so rel stops as well.

**Cause**
`init_io_apic()` validates only half of an override. Since `fix(x86-apic): resolve the last pin of an IO-APIC to it` (`docs/changes/261004/x86-apic.md`) it refuses, with a warning, an override whose ISA number is 16 or more, following Linux and the plan's "refuses". It still stores an override whose ISA number is fine but whose GSI is unowned, so the bad value reaches `apic_configure_irq()`, whose ASSERT is meant for a kernel caller's bug, not for firmware input. The two firmware checks therefore disagree: one bad field is refused and the boot goes on, the other bad field stops the boot.

This matches Zircon, which panics in `resolve_global_irq()` when its `platform_init_apic()` programs the same ISA lines, so it is not wrong against the default spec; it is inconsistent with the Linux behaviour step 1 chose for the other field. The step-1 change record defers the decision to step 7 of `.agents/tasks/microkernel-interrupts/PLAN.md`, which rewrites the loop that programs the ISA lines, but that step does not list it; this item is what carries it there.

**Change**
A decision for the user, since one option departs from Zircon:
- (a, recommended) refuse the override in `init_io_apic()`, next to the ISA-number check: skip it with a `log::warn` when `resolve_irqnum_to_ioapic(isa_override.global_irq)` is null, so that the ISA line falls back to the identity mapping. This keeps every firmware check in one place and leaves `apic_configure_irq()`'s ASSERT for caller bugs. It runs after the IO-APIC setup loop, where the lookup works.
- (b) keep Zircon's panic and write the departure from Linux down in a comment at the ISA-number check, so the two checks' different answers are explained rather than accidental.

**Reference**
- Linux `mp_register_ioapic_irq()` (arch/x86/kernel/acpi/boot.c), called for each override through `mp_override_legacy_irq()`: `mp_find_ioapic(gsi)` fails, it warns "Failed to find ioapic for gsi" and returns, so the override is dropped and the boot goes on.
- Zircon `resolve_global_irq()` (zircon/kernel/arch/x86/src/ioapic.rs), reached from `apic_io_configure_isa_irq()` in `platform_init_apic()` (zircon/kernel/platform/pc/interrupts.rs): panics with "Could not resolve global IRQ".

**Acceptance**
- With option (a): an override with an unowned GSI, injected by hand into the list `platform_init_apic()` collects (not committed), logs the warning and both images still boot; the ISA line is then programmed at its own number.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes on both images, and the dbg log's override table is unchanged on q35.
