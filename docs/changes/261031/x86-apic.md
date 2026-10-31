# x86-apic changes

## Every IO-APIC pin is programmed masked and has an interrupt line (2026-10-31)

**Files**
- `kernel/platform/pc/interrupt.cpp` — `platform_init_apic()` programs, after the ISA lines, every other pin of every IO-APIC, masked, with vector `PlatformIrqMin + GSI`, level-triggered and active low, and builds a line for each. The GSIs collected for `irq::init_lines()` move from a 16-entry array to a `ktl::Vec`; the comments say what is true now; `<array>` is no longer included.
- `kernel/arch/x86/apic/io_apic.cpp`, `kernel/arch/x86/kernel/include/ours/arch/apic.hpp` — new `registered_io_apics()`, the read-only list of the IO-APICs with their pin counts.
- `kernel/commands/pin_lines_test.cpp`, `kernel/commands/BUILD.gn` — new kshell test `pinlines.test`.
- `docs/repairs/261031/x86-apic.md` — new: the one item this change leaves, a pin the firmware wired to NMI.

**Symptom**
`platform_init_apic()` programmed only the pins the ISA lines reach, and `irq::init_lines()` built a line for those and no others. A PCI function's INTx comes in on one of the other pins (GSI 16 to 23 on q35), which had no line: `interrupt_create` on it answered NoResource, and its entry was left as the firmware had left it. With `DISK=` set (the previous change) there is a device whose interrupt could not be taken.

**Cause**
Not a mistake: the first version registered what ISA needed and said so (`.agents/tasks/irq-genirq/DESIGN.md` D4, `.agents/tasks/driver-capabilities/DESIGN.md` R4). PCI is the user that needs more. The objects involved:

- An IO-APIC pin, one redirection entry: vector, delivery mode, trigger mode, polarity, mask bit. The pin's number is its GSI, the IO-APIC's `gsi_base()` plus its index.
- A line, the irq layer's descriptor for a GSI. The kernel names a line by the GSI and gives the pin the vector `PlatformIrqMin + GSI`, so a vector read in the interrupt entry is turned back into the line by subtracting the base. A pin with an entry but no line, or a line but no entry, would break that one-to-one.
- The platform's own list of IO-APICs. `platform_init_apic()` collects descriptors from the MADT in a local vector, and `init_io_apic()` copies them into the arch layer's `IOAPICS` and only then reads each chip's version register, where the highest redirection index (the pin count) is. The local descriptors never learn it. That is why the platform could not simply loop over its own list, and why the arch layer now hands out its list.

Two choices in the entry that is written. Masked, because an unmasked pin with nobody on it would deliver into a line with no action; the irq layer's flow would mask it again, but the first interrupt on a level pin that is still asserted is better never taken. Level and active low, because that is how PCI signals and these pins have no other use; the MADT says nothing about them (its overrides are for ISA lines). Whoever takes a line passes the mode it wants to `interrupt_create`, which configures the pin as the line is taken, so this is only what a line is until then, and what `ModeDefault` leaves.

**Change**
The second loop, after the ISA one:

```cpp
        for (auto const &ioapic: registered_io_apics()) {
            for (auto pin = usize{0}; pin < ioapic.irq_count(); ++pin) {
                auto const gsi = static_cast<HIrqNum>(ioapic.gsi_base() + pin);
                if (is_programmed(gsi)) {
                    continue;
                }

                if (gsi > MAX_GSI) {
                    log::warn("IO-APIC[{}] pin {} is GSI {}, past the platform vectors; no line",
                              ioapic.id(), pin, gsi);
                    continue;
                }

                auto const vector = IrqVec(usize(IrqVec::PlatformIrqMin) + gsi);
                apic_configure_irq(gsi, ApicTriggerMode::Level, ApicInterruptPolarity::ActiveLow,
                                   ApicDeliveryMode::Fixed, true, ApicDestinationMode::Physical,
                                   static_cast<u8>(bspid), vector);
                remember(gsi);
            }
        }
```

`is_programmed()` is what the ISA loop already did with its array, now over the vector, and it is why a pin an ISA line reached keeps the entry the MADT's override gave it. A GSI past the platform vectors (207) is skipped with a log line and the pin is left as the firmware left it; q35's 24 pins never reach that.

Rejected: reading the pin count where the descriptors are made (the platform's local copies are not the ones that read the chip); making the platform vector a fixed-size array big enough for every pin (the count is the firmware's, and a machine with several IO-APICs has more than any bound written here); programming the pins in `init_io_apic()` (that is the arch layer's job of mapping and reading the chips, and which vector a pin gets is the platform's policy).

**Reference**
None, this project's own design: `.agents/tasks/interrupt-system/PLAN.md` step I2. The vector rule `PlatformIrqMin + GSI` is the platform's existing one.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- Build: ok, 47 s for the incremental build after the header change. (Only the end of the build output was read, so no claim is made about warnings beyond what clang-tidy below reports.)
- `pinlines.test`: ok, 14 checks. 24 pins, 24 with a line, 0 past the platform vectors, 8 pins from the ISA range up (GSI 16 to 23), all masked and level. Also ok, 14 checks and the same counts, with `DISK=` set.
- The four existing interrupt tests have the same number of checks as before: `interrupt.test` 32, `interruptline.test` 58, `irq.test` 72, `resource.test` 60, all ok.
- `boot_check.py`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required (the keyboard's keys still arrive through the platform's pins); host tests 151 passed, 0 failed. The same with `DISK=` set.
- Mutation: the second loop made to skip every GSI from 16 up. `pinlines.test` then fails (`FAILED -- 4 of 9 checks`, 30 failed or error lines: the eight pins have no line, are not masked, do not signal level, and an object cannot take the last pin: NoResource). Restored, and the diff was byte-for-byte what it was before the mutation.
- `scripts/format_cpp.py --check` and `scripts/format_gn.py --check` clean on every changed file. clang-tidy 23.1.0-rc3 against the build's `compile_commands.json`: no diagnostics in the four changed C++ files (90, 101 and 46 in headers that were not touched).

Not measured: a machine with more than one IO-APIC, one with more than 24 pins, a pin past the platform vectors (the skip is not run by any machine here), real hardware, QEMU 11.1.1.

**Deliberately not done**
- Per-pin trigger and polarity from the firmware. There is none for these pins; `_PRT` has it and no component reads `_PRT` yet.
- Pins wired to NMI by the firmware: the MADT's NMI source entries are not read, so such a pin would now be overwritten. QEMU has none. Recorded in `docs/repairs/261031/x86-apic.md`.
- Dynamic vectors: the vector is still `PlatformIrqMin + GSI` (interrupt-system D8). Several IO-APICs with more than 208 pins in all would lose lines; the warning names them.
- A syscall or virq change: `interrupt_create` still takes the GSI. That is the next step.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why could `platform_init_apic()` not loop over the IO-APIC descriptors it already had, and what did it need instead?
2. A pin is programmed masked, level and active low, and a driver then asks for it with `ModeLevelLow`. What does the create change, and what would it have changed for a driver that asked for `ModeEdgeHigh`?
3. What does the mutation show that the 14 passing checks do not?
