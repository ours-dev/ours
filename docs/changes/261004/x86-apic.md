# x86-apic changes

## Title: The last pin of an IO-APIC resolves to it, and the ISA override table logs and checks each entry (2026-10-04)

**Files**
- `kernel/arch/x86/apic/io_apic.cpp` — `resolve_irqnum_to_ioapic()` asks `IoApic::owns_global_irq()`; `init_io_apic()` checks that each IO-APIC's first and last GSI resolve to it, refuses an override whose ISA number is past the table, and logs each override's own entry; `apic_configure_isa_irq()` asserts its ISA number is in range.

**Symptom**
Two of the three were latent, one was visible:
- Latent, cannot be triggered today: GSI 23, the last pin of q35's 24-pin IO-APIC, resolves to no IO-APIC, so `apic_mask_irq(23)` or `apic_unmask_irq(23)` hits a DEBUG_ASSERT on dbg and dereferences null on rel, and `apic_configure_irq(23, ...)` panics on its ASSERT. On a machine with a second IO-APIC, none of its pins resolve. Today's callers name only GSIs 0 to 15, so nothing reaches it yet; step 7 of `.agents/tasks/microkernel-interrupts/PLAN.md` names lines by GSI and step 11 lets a user process name one, at which point the first request for the last pin would crash the kernel (DESIGN.md defect 3).
- Visible on every dbg boot: the ISA override table showed one real row and four phantom ones. q35's MADT has five overrides (ISA 0 to GSI 2, and ISA 5, 9, 10 and 11 level-triggered), and the log printed:

```text
ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ
0      | true | edge | high | 2
0      | false | edge | high | 0
0      | false | edge | high | 0
0      | false | edge | high | 0
0      | false | edge | high | 0
```

- Latent: an override whose ISA number is 16 or more would be written past the end of the 16-slot table.

**Cause**
The lookup compared two numbers from different spaces with the wrong kind of bound:

```cpp
if (IOAPICS[i].gsi_base() <= irqnum && irqnum < IOAPICS[i].max_local_irqnum()) {
```

`irqnum` is a global system interrupt (GSI): ACPI numbers every IO-APIC pin in the machine in one flat space, and each IO-APIC owns the slice that starts at its `gsi_base` (from its MADT entry). `max_local_irqnum()` is something else entirely: bits 16-23 of the IO-APIC's version register, the "maximum redirection entry", which is the index of its last pin, numbered from 0 within that IO-APIC. So the test is wrong twice over:
- **Inclusive maximum used as an exclusive bound.** A 24-pin IO-APIC reports 23. `irqnum < 23` drops pin 23, the last one. The register's name says "maximum", not "count", and Linux's `io_apic_get_redir_entries()` adds 1 to it for exactly this reason.
- **A local index compared with a global number.** The upper bound forgets to add `gsi_base`. For the first IO-APIC, whose base is 0, the two spaces coincide, which is why only the last pin was lost. For a second IO-APIC with base 24 and the same 24 pins, the test becomes `24 <= irq && irq < 23`, which nothing satisfies: every one of its pins is unreachable, not just the last.

The correct test already existed in the arch library, next to the two numbers it is made of: `IoApic::owns_global_irq()` computes `irq - base < irq_count()`, where `irq_count()` is the maximum plus one. The lookup in the kernel had simply been written by hand instead of asking the object that owns the range, so the knowledge "the register is an inclusive index, and pins are numbered from the base" had two owners and one of them got it wrong.

Nothing noticed because nothing has asked for a GSI above 15 yet: the ISA lines are programmed at boot, the PIT uses GSI 2 and the keyboard GSI 1. That is also why the fix has to land first in this plan: every later step that names a line by GSI goes through this lookup.

The override log read the wrong table slot. The loop walked the MADT's list of overrides with an index `i`, and stored each one where it belongs, at its ISA number:

```cpp
IOAPIC_ISA_OVERRIDES[overrides[i].isa_irq] = overrides[i];
log::trace(..., IOAPIC_ISA_OVERRIDES[i].isa_irq, ...);
```

but then logged slot `i` of the ISA-indexed table, mixing the list's index with the table's. The first override happens to be ISA 0, so row 0 was right by coincidence; rows 1 to 4 printed the untouched default slots 1 to 4. The table itself was correct, only its log lied, but a log that hides four level-triggered lines is exactly what one reads when an interrupt does not arrive.

The same store indexes a 16-entry `std::array` with the MADT's `source` byte, which can hold 0 to 255. `std::array::operator[]` does no checking, so a malformed entry (firmware bugs in the MADT are common enough that Linux checks for them) would have written past the table into whatever static data follows it.

**Change**
The lookup asks the IO-APIC:

```cpp
// before
for (auto i = usize{0}; i < IOAPICS.size(); ++i) {
    if (IOAPICS[i].gsi_base() <= irqnum && irqnum < IOAPICS[i].max_local_irqnum()) {
        return &IOAPICS[i];
    }
}
return 0;

// after
for (auto &ioapic: IOAPICS) {
    if (ioapic.owns_global_irq(irqnum)) {
        return &ioapic;
    }
}
return nullptr;
```

After every IO-APIC is set up, `init_io_apic()` checks that both ends of each one's range resolve back to it:

```cpp
for (auto const &ioapic: IOAPICS) {
    auto const first = static_cast<HIrqNum>(ioapic.gsi_base());
    auto const last = static_cast<HIrqNum>(ioapic.gsi_base() + ioapic.irq_count() - 1);
    DEBUG_ASSERT(resolve_irqnum_to_ioapic(first) == &ioapic, ...);
    DEBUG_ASSERT(resolve_irqnum_to_ioapic(last) == &ioapic, ...);
}
```

The last pin catches an off-by-one bound, the first a forgotten base, and either one resolving to another IO-APIC means the firmware gave two of them overlapping ranges. It runs after the setup loop, not inside it, because a default-constructed `IoApic` slot has base 0 and maximum index 0, so it claims GSI 0 until it is initialised. The plan asked for the last pin only; the first costs one more comparison and covers the second half of the bug.

The override loop iterates the overrides themselves, refuses an out-of-range one, and logs the entry it stored:

```cpp
for (auto const &isa_override: overrides) {
    if (isa_override.isa_irq >= arch::NUM_ISA_IRQS) {
        log::warn("IO-APIC: ignoring an override of ISA IRQ {}, past the {} ISA lines", ...);
        continue;
    }
    IOAPIC_ISA_OVERRIDES[isa_override.isa_irq] = isa_override;
    log::trace("{: <6} | {} | {} | {} | {}", isa_override.isa_irq, ...);
}
```

Refusing one entry and booting on, rather than asserting, follows Linux: the MADT is firmware's input, and one bad override of a line nobody may use is no reason to stop the boot. Zircon asserts here instead (`apic_io_init_safe()`); that was rejected because the plan asks for the entry to be refused, and because a panic over an unused line trades a working machine for nothing.

`apic_configure_isa_irq()` indexes the same table with its argument, so it now asserts the number is in range, as Zircon's `apic_io_configure_isa_irq()` does. Its callers pass constants 0 to 15, so a bigger number is a caller's bug, not input to tolerate; the plan did not list this check, but it is the same unchecked index into the same array.

**Reference**
- Linux `mp_find_ioapic()` (arch/x86/kernel/apic/io_apic.c) tests `gsi >= gsi_cfg->gsi_base && gsi <= gsi_cfg->gsi_end`, an inclusive end computed once in `mp_register_ioapic()` as `gsi_base + io_apic_get_redir_entries() - 1`, where `io_apic_get_redir_entries()` is the version register's maximum plus one. `mp_register_ioapic()` also refuses an IO-APIC whose range overlaps one already registered, which is what the new boot check catches as an assertion.
- Zircon `resolve_global_irq_no_panic()` (zircon/kernel/arch/x86/src/ioapic.rs) tests `start <= irq && irq <= start + max_redirection_entry`, the same inclusive end.
- Linux `mp_override_legacy_irq()` (arch/x86/kernel/acpi/boot.c) warns "Invalid bus_irq %u for legacy override" and returns for a source of 16 or more; Zircon `apic_io_init_safe()` asserts `isa_irq < NUM_ISA_IRQS` and `apic_io_configure_isa_irq()` asserts the same on its argument.

**Verification**
- `boot_check.py`: dbg PASS (shell finished at 14.2 s, 24 passed, 1 skipped, 0 failed, 0 error lines); rel PASS (3.5 s, 22 passed, 3 skipped, 0 failed, 0 error lines). The dbg image runs the new DEBUG_ASSERTs.
- The override table on dbg after the change, each override on its own row:

```text
ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ
0      | true | edge | high | 2
5      | true | level | high | 5
9      | true | level | high | 9
10     | true | level | high | 10
11     | true | level | high | 11
```

- The boot check catches the old bug: with the new file but the old bound restored in `resolve_irqnum_to_ioapic()` (not committed), dbg panics during init with `Assertion failed on resolve_irqnum_to_ioapic(last) == &ioapic with message: IO-APIC[0]: its last GSI 23 does not resolve to it`.
- clang-format: clean. clang-tidy: no finding on the changed lines; the nine `readability-identifier-naming` findings it reports (`IOAPICS`, `IOAPIC_CHIP`, `IOAPIC_ISA_OVERRIDES`, `IoApicChipData`'s members) are on lines this change did not touch.
- Not tested: the refusal of an out-of-range override (q35's MADT has none) and a second IO-APIC (q35 has one).

**Deliberately not done**
- `apic_io_isa_to_global()` still scans the 16 slots for a remapped entry with that ISA number rather than indexing, as Zircon's does after an assertion. It is correct as it is, and a number of 16 or more comes back unchanged; whether a user's REMAP request with such a number is refused belongs to step 11, which is where user input first reaches it.
- The table's header is logged at info and its rows at trace, so the rel image shows a header with no rows. The plan only asked for the right rows; the levels were left as they were.
- `IoApicChipData` and the chip operations that walk its `pins_` are untouched: step 8 replaces them with a lookup by `data.hirqnum`, which goes through the function fixed here.
- `max_local_irqnum()` keeps its name. It is accurate (the highest local pin number), and after this change only the debug dump reads it.

**How it was found**
From the design review behind `.agents/tasks/microkernel-interrupts/DESIGN.md` (defect 3): reading `io_apic.cpp:107` against `ioapic.hpp:112-124`, where `max_local_irqnum()` returns the version register's maximum and `owns_global_irq()` already did the right arithmetic. The override log bug came from reading the loop at `io_apic.cpp:210-216`, and the before and after logs above confirm it. The lookup bug was confirmed by booting the old bound against the new assertion.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did the old test lose every pin of a second IO-APIC, but only the last pin of the first one?
- Why does the boot check run after all IO-APICs are initialised rather than as each one is set up, and what would a default-constructed slot do to it?
- Why does `init_io_apic()` refuse a bad override and keep booting, while `apic_configure_isa_irq()` asserts on a bad ISA number?
