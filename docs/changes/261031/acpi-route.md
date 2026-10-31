# acpi-route changes

## The route service's logic: a reader of the tables' directory, the GSI of an ISA line, and the window of a PCI segment (2026-10-31)

**Files**
- `system/service/acpi-route/include/acpi_route/table_directory.hpp`, `src/table_directory.cpp` — new, `TableDirectory`: reads the VMO of <ours/acpi_tables.hpp> and refuses anything that does not hold.
- `system/service/acpi-route/include/acpi_route/routes.hpp`, `src/routes.cpp` — new, `isa_irq_to_gsi()` and `ecam_window()`.
- `system/service/acpi-route/BUILD.gn` — new, the headers, the `logic` source set, the host test and its group.
- `system/service/acpi-route/test/acpi_route_test.cpp` — new, 16 cases.
- `kernel/lib/acpi/include/acpi/structures/mcfg.hpp` — new, the MCFG's layout (data only), listed in `kernel/lib/acpi/BUILD.gn`.
- `BUILD.gn` — the test group joins the default build, so `boot_check.py` runs the test.

**Symptom**
Latent, cannot be triggered today: no program but the kernel can say which GSI an ISA interrupt arrives on, or where a PCI segment's configuration space is. `docs/repairs/261031/syscall.md` item 1 (`Isa` and `kind` leaving `interrupt_map`) waits for a user-space answer to the first, and the ECAM base is written into `routing_checks.cpp` as a constant.

**Cause**
Not a mistake: the answers existed only inside the kernel, and the previous change gave user space the tables to work them out from. Four details decide what the functions say.

- **An ISA line is not a GSI.** A PC's ISA interrupts 0 to 15 arrive on the IO-APIC pins the chipset wired them to. By default pin N carries ISA N, but not always: q35 sends ISA 0, the timer, to pin 2. The MADT says so in an *interrupt source override* — bus, ISA number, GSI, and flags for how the line signals — and the answer for an ISA number is the first override of the ISA bus that names it, or the same number when there is none. The flags are two two-bit fields, polarity and trigger mode, each with a value that means "conforms to the bus", and for an ISA line that is edge triggered and active high; a reserved value is something this cannot honour, so it is refused, where the kernel would panic.
- **Two numbers are not lines.** ISA 2 is the cascade between the two PICs. q35 leaves it on the timer's pin, so naming it would hand out the timer under a second name; the kernel leaves it out of `platform::isa_irq_line()` for that reason, and this does the same. A number past 15 is no ISA line either. The kernel's lookup takes a `u8`, which is why its bound comes first; this takes a `u32` and compares it as one.
- **A table that cannot be read to its end cannot say there is no override.** The MADT's entries are walked until the first malformed one. If the walk ended there without finding the ISA number, the override may be just after it, and answering "none" would name the wrong line. So that is an error, and an override found before the bad entry is still an answer.
- **The MCFG's base address is where bus 0 would be.** An allocation gives a base, a first bus and a last bus, and the space of bus N is at `base + (N << 20)`, whether or not the segment has a bus 0. The window of buses 16 to 31 therefore starts 16 MiB past the base, and `EcamWindow::address` is that start. A caller that mapped the base would read the wrong buses.

The reader of the directory is what makes the rest safe to write. The bytes are the firmware's, by way of the kernel, and the program that reads them must never read outside them: `from_bytes()` checks the header, every entry's bounds, alignment and order, that the table's own header says what the entry says, and the checksum, before anything is handed out; `find()` can then cut a table from the bytes without a second look. The limit on the count (256) bounds the walk of a corrupt count; it cannot be told apart from the other checks by a real machine, so a test builds a directory of exactly that many tables by hand.

Where it lives: the functions are in the route service's directory, not in `libacpi`, which the user chose over the alternative on 2026-10-31 (only user space asks, and the kernel keeps its own translation). The MCFG layout is plain data like every other table's and is in `libacpi`.

**Change**
The core of the ISA answer, the first override of the ISA bus that names the line:

```cpp
for (auto const &entry: entries) {
    if (entry.bus != ISA_BUS || entry.source != isa_irq) { continue; }
    auto route = InterruptRoute{.gsi = entry.global_irq, .trigger = Trigger::Edge,
                                .polarity = Polarity::ActiveHigh};
    // Level and ActiveLow change the defaults; a reserved value returns InvArgs.
    return ustl::ok(route);
}
if (entries.malformed()) { return ustl::err(Status::InvArgs); }
return ustl::ok(InterruptRoute{.gsi = isa_irq, ...});   // no override: its own number
```

and the window of a segment: `address = base_address + (start_bus << 20)`, `size = (end_bus - start_bus + 1) << 20`, refused when the last bus is before the first or the window would run past the end of the address space.

**Reference**
The kernel's own `apic_io_isa_to_global()` (`kernel/arch/x86/apic/io_apic.cpp`) and `parse_io_apic_isa_overrides()` (`kernel/platform/pc/interrupt.cpp`): the first override that names the line, the same defaults for "conforms". Linux handles the overrides in `acpi_parse_int_src_ovr()` and `mp_override_legacy_irq()` (`arch/x86/kernel/acpi/boot.c`, read in the earlier survey of its interrupt path). For the MCFG, Linux's `pci_mmconfig_alloc()` in `arch/x86/pci/mmconfig-shared.c` (master, read for this entry): `res->start = addr + PCI_MMCFG_BUS_OFFSET(start)`, with `PCI_MMCFG_BUS_OFFSET(bus)` being `bus << 20`.

**Verification**
- `acpi-route-test`, 16 cases, over directories built the way the kernel builds them (the library's test machine, then `acpi::export_tables()`): a directory read back with the DSDT first; 16 ways to refuse a bad one (the header, each field of an entry, two entries for one table, a bad checksum, and the count limit built by hand at 256 and 257); an ISA line with no override, with q35's overrides, with edge/level and polarity in each field, with the first of two overrides winning, with an override on another bus, the cascade and the numbers past 15, reserved flags, no MADT, and a MADT whose entries end in a bad one; the window of two segments including one that starts at bus 16, a half-written allocation, a last bus before the first, two wrapping windows, and no MCFG.
- Eighteen mutations, each caught by a test and each restored byte for byte: the bus unchecked, the level trigger and the low polarity lost, a reserved trigger accepted, the malformed MADT ignored, the cascade or the numbers past 15 made lines, the first bus left out of the window, a backwards or wrapping allocation accepted, a partial allocation read, the segment unmatched, the checksum, the overlap, the header's signature, the size and the reserved word unchecked, and the count unbounded. Three of the first run's tests did not catch their mutation and were changed: the overlap case (a second entry for the same table was refused for its header, not for the overlap), the half-written allocation (eight bytes did not reach the segment field), and the count limit (the entries' size refused it first).
- Compiled for user space with the flags of a real user-space source (`-ffreestanding`, `-fno-exceptions`, `-fno-rtti`, `-Werror`): both sources, and `llvm-nm -u` says they need only `memcpy` besides their own class. clang-tidy: no findings.
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 153 passed (152 before: the new executable).
- Not verified: no user program links this yet, so the build file's user-toolchain wiring is untested; the tables of real hardware are not seen; the answers are not yet compared with the kernel's, which the program that serves them does with `routing_checks`.

**Deliberately not done**
No program, no protocol and no handle to the VMO: this is what the program will answer with. No `_PRT` or `_CRS`, which need AML. The kernel's translation is not changed to use this: two independent implementations that must agree are worth more for now than one. The reserved flag values are refused where the kernel panics, a difference that real firmware does not exercise.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the answer for ISA 0 on q35 GSI 2, and why is the answer for ISA 2 "no line" although the same override table could name it?
- Why does a MADT whose entries end in a malformed one give an error for an ISA number it did not find, and a normal answer for one it did?
- An MCFG allocation says base 0x4_0000_0000 and buses 16 to 31. Where does the window start, and what goes wrong for a driver that maps the base?
