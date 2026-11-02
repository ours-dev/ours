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

## The route service is a program, and routing_checks makes its answers agree with the kernel's (2026-10-31)

**Files**
- `system/service/acpi-route/src/acpi_route.cpp` — new, the program: maps the tables, serves `ours.acpi.Route` on a Loop through `ioservice::Outgoing`, answers each request with `isa_irq_to_gsi()` or `ecam_window()`.
- `system/service/acpi-route/BUILD.gn` — the user-only part: the program and `acpi-route.cm`, a manifest that uses `ours.boot.Acpi` and exposes `ours.acpi.Route`.
- `kernel/products/image/dev/BUILD.gn` — `acpi-route` as an eager child of `root.cm`, the tables offered to it, and its protocol offered to routing_checks.
- `system/lib/ours.acpi/include/ours_acpi/client.hpp`, `src/client.cpp`, `BUILD.gn` — the client: `isa_irq()` and `ecam_base()`, one request on a connection of its own.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `BUILD.gn` — `check_acpi_route()` and the use of the protocol.

**Symptom**
Latent, cannot be triggered today: the answers of the previous changes had no program to give them, and nothing compared them with the kernel's. `docs/repairs/261031/syscall.md` item 1 (`Isa` and `kind` leaving `interrupt_map`) waited for a user-space component that could say which GSI the keyboard is on.

**Cause**
Not a mistake: a missing program, and what it is allowed to hold decides its shape. Four details decide it.

- **It holds the tables and nothing else.** Its manifest uses one capability, `ours.boot.Acpi`, and `root.cm` offers it nothing more: no resource root, no I/O. The bytes it reads are the firmware's, and the logic it runs on them has been tested against crafted ones, but a bug in it would still be a bug in a program that reads untrusted bytes. With only a read-only VMO to hold, the worst that bug does is to take the program down or make it answer wrongly, and a wrong answer is a misrouted interrupt, not access to a device. That is the invariant the Driver Manager's design asks of the party that parses firmware.
- **No heap, and no copy.** A user program has no heap yet (`system/lib/ort/src/cpp_dependencies.cpp`). Its clients, buffers and slots are `constinit` storage built in `main()`, as the echo server's are, and the tables are read where they are mapped: `TableDirectory` keeps a span over the mapping and cuts tables out of it, so nothing is allocated for them.
- **No tables, no program.** If the VMO cannot be had, mapped or read as a directory, the program says why and exits with 1. A route to a component that has gone answers NotFound, which is the truthful answer from a route service that has nothing to route by; staying up to answer NotFound to every question would look like a service that read the tables and found no line.
- **Two translations that must agree.** The kernel turns an ISA number into a line from the MADT; so does this program, written apart, from the copy of the same tables. `check_acpi_route()` makes them meet: for each ISA number 0 to 15 but the cascade, the GSI the service gives, put to `interrupt_map` as a Gsi, must name the line `interrupt_map` names for the ISA number. If either side had a wrong rule for overrides, a wrong default, or a wrong bound, the two would name two lines for one number. The service's ECAM address for segment 0 is compared with the address this program already maps QEMU's window at, which was written into the test by hand, so it is a third, independent source.

What the check found on its first run was a wrong expectation of mine, not a wrong answer: for 0xffffffff the kernel answers InvArgs, not NoResource, because it holds that number back as its own "no such line" (`irq::INVALID_H_IRQ_NUM`), while to the service it is only a number that is no ISA line (NotFound). The check now says so, and the two are allowed to differ there.

**Change**
The shape of a request in the server, as the echo server's:

```cpp
auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
if (header.is_ok() && header->ordinal == ours_acpi::ISA_IRQ.ordinal) {
    if (auto const request = ours_acpi::decode_isa_irq(message); request.is_ok()) {
        return isa_irq(client, *request);
    }
} // ... EcamBase the same; anything else closes the connection
```

and the check, for one ISA number:

```cpp
auto const route = ours_acpi::isa_irq(isa);                       // the service
ours::interrupt_map(irq, InterruptSourceKind::Isa, isa, &by_isa); // the kernel
ours::interrupt_map(irq, InterruptSourceKind::Gsi, route->gsi, &by_gsi);
if (by_gsi != by_isa) { fail("... name two lines for one ISA number"); }
```

**Reference**
The echo server (`examples/echo/echo_server.cpp`) is the shape of the program, and `ours_resource::get()` (`system/lib/ours.resource/src/client.cpp`) that of the client. For what the program is, `.agents/tasks/driver-manager/DESIGN.md`, the route service; the kernel's translation it is checked against is `platform::isa_irq_line()` (`kernel/platform/pc/interrupt.cpp`).

**Verification**
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 154 passed. The root starts the program (`init: started acpi-route`), which says `acpi-route: serving, from 7 ACPI tables`, and `routing_checks: passed`. On this machine every ISA number 0 to 15 but the cascade names the same line both ways, ISA 0 being on GSI 2 (`check_interrupt_map()`), and the ECAM window for segment 0 is at 0xE0000000.
- Seven mutations, each booted on the rel image and each caught by `routing_checks` (the number of FAILED lines in brackets): the service answering a GSI one too high (16), serving under another name (17), mapping nothing (17), the protocol not offered to routing_checks (17), the overrides ignored (2), the ECAM address one byte off (2), and the cascade made a line (2). The files were restored byte for byte, and the final run above is of the restored sources.
- clang-tidy on the program and the client: no findings.
- Not verified: the dbg image runs none of this (its log has no user-space output, as the previous record says), so the check runs in rel alone; any firmware but QEMU's q35 under edk2, and so any MADT whose overrides differ from this one's; the service under a client that sends a malformed message or connects more than four at a time, which only the codec's tests and the code cover.

**Deliberately not done**
pc-ps2 does not ask the service yet: it still names ISA 1 to `interrupt_map`, and `Isa` and `kind` stay until it does (`docs/repairs/261031/syscall.md`). No second client, no restart of the program (the framework starts no exited component again), no `_PRT` or `_CRS`. The `ECAM_BASE` of `routing_checks` stays a constant that the service is checked against, not a value taken from it.

**How it was found**
None, a planned step. The one failure of the first run was read from the line `routing_checks: interrupt_map(Isa, past the ISA lines) = InvArgs (expected NoResource) FAILED` in the rel log.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the check give the service's GSI to `interrupt_map` as a Gsi, and not compare the two numbers directly?
- Why does the program exit when it has no tables, and not stay up to answer NotFound?
- The kernel answers InvArgs for 0xffffffff and the service NotFound. Why is that a difference to allow and not a bug?

## The route service follows the kernel's rules for overrides, the reader's checks stand alone, and routing_checks holds QEMU's whole ISA table (2026-10-31)

**Files**
- `system/service/acpi-route/src/routes.cpp`, `include/acpi_route/routes.hpp` — `isa_irq_to_gsi()` follows the kernel's rules: the last override of a line wins, the bus field is not read, a malformed tail ends the walk and what was seen stands.
- `system/service/acpi-route/src/table_directory.cpp`, `include/acpi_route/table_directory.hpp` — the cap of 256 tables is gone.
- `system/service/acpi-route/test/acpi_route_test.cpp` — the cases of the three rules, and a test that each check of the reader refuses by itself.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `BUILD.gn`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/pools.hpp`, `init.cpp`, `system/driver/pc-ps2/pc_ps2.cpp`, `system/public/ours/syscalls.gni` — the QEMU table check, and comments that had gone false.
- `.agents/tasks/acpi-route/PLAN.md`, `driver-manager/DESIGN.md`, `interrupt-system/PLAN.md`, `driver-capabilities/RESULT.md` — sentences that had gone false.
- `docs/repairs/261031/acpi-route.md` — new: what the review found and this change leaves.

**Symptom**
Latent on QEMU, wrong on a table QEMU does not produce. An independent review of the ACPI work (four reviewers, one skeptic for each, then two more on the keyboard and ABI commits) found three places where the route service named a different pin than the kernel had programmed, and a set of checks that the tests did not hold in place.

**Cause**
The point of the service is that a driver binds the pin the kernel programmed for the ISA number. The kernel reads the MADT's overrides by three rules, and the service had been written to three others.

- **Two overrides of one number.** The kernel stores each override at its ISA number (`IOAPIC_ISA_OVERRIDES[isa_override.isa_irq] = isa_override`), so a later entry replaces an earlier one and the last wins. The service returned at the first. With `{ISA 4 -> GSI 10}` then `{ISA 4 -> GSI 11}` it answered 10 where the kernel programmed 11, and a driver that took the service's answer would have bound a pin nothing was wired to. The record of the service said the kernel took the first; it does not.
- **The bus field.** The kernel never reads it (the MADT fixes it at 0). The service skipped an entry whose bus was not 0, so a non-conforming table the kernel honours was ignored by the service.
- **An unreadable tail.** The kernel's walk of the entries ends at the first malformed one and does not look at `malformed()`. The service answered InvArgs when it had not found the line before that point, a stricter rule: on firmware with one stray byte after the entries the keyboard worked through `interrupt_map(Isa)` and would not start through the service. Answering the way the kernel does costs nothing the kernel does not already lose.

These were decisions of mine that I wrote down as deliberate; the review showed the cost of each, and what the service is for outweighs any of them. The reserved flag values stay an error here where the kernel panics: that difference is on a path the kernel does not survive.

The reader's checks were the second finding. Six of the eight checks of `TableDirectory::from_bytes()` could be removed one at a time and the test still passed, because each case broke the directory in a way that more than one check refuses: a table past the directory's size also has a header that disagrees, an unaligned offset also overlaps. The cases now break one thing and make every other check hold: the entry and the table's own header patched together and the checksum sealed again, a hand-made directory whose stride puts a table four bytes past a multiple of 8, and for the check that only keeps the reader inside its bytes, a directory placed at the end of a mapped page with an unmapped page after it, so that a read past the end is a crash and not a quiet read of whatever follows. That last way needs no sanitizer, and it showed the mutant that every other test let through. The cap on the count of tables was a bound the checks already gave (the entries must fit in the size, and the tables may not overlap), and refusing 257 tables stopped the whole service on a machine with many SSDTs, so it went.

The third was the trigger and the polarity. The service answers them, and nothing read them: a swap of the two arms of the mapping in the program passed the whole boot. QEMU's tables, as the kernel prints them when it programs the pins, are a fixed fact of this machine (the timer is moved to GSI 2; ISA 5, 9, 10 and 11 are level triggered; every other line is its own GSI, edge, active high), and `check_acpi_route()` now holds the service to all fifteen of them. The check is specific to QEMU's tables, as the existing checks of `interrupt_map` are, and says so.

**Change**
The walk:

```cpp
auto route = InterruptRoute{.gsi = isa_irq, .trigger = Trigger::Edge, .polarity = Polarity::ActiveHigh};
for (auto const &entry: acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt)) {
    if (entry.source != isa_irq) { continue; }
    route = /* gsi, trigger and polarity of this entry */;   // a later entry replaces an earlier
}
return ustl::ok(route);
```

and the unaligned-table case, a stride of 44 bytes for tables of 36, where the second table is four bytes past a multiple of 8 but after the end of the first:

```cpp
auto const unaligned = hand_made_directory(2, 44, 36);   // only the alignment check refuses it
```

**Reference**
The kernel's own `apic_io_isa_to_global()` and `parse_io_apic_isa_overrides()` (`kernel/arch/x86/apic/io_apic.cpp`, `kernel/platform/pc/interrupt.cpp`), the rules this now follows. Linux's `mp_override_legacy_irq()` (`arch/x86/kernel/acpi/boot.c`, read in the earlier survey) also ends by assigning `isa_irq_to_gsi[bus_irq] = gsi`, so a later override of the number replaces an earlier one, and its `acpi_parse_int_src_ovr()` does not read the bus. The earlier record's Reference named these functions as the model for first-wins and bus-0-only, which they are not.

**Corrections to earlier records**, which are history and are not rewritten:
- `docs/changes/261031/acpi-route.md` (the first entry): "the first override of the ISA bus that names the line" and "the kernel's `apic_io_isa_to_global()`: the first override that names the line" are wrong as written; the kernel's is the last. Its "two independent implementations" overstates it: both read the MADT through the same `acpi::entries` walker and flag decoders, so a fault there is common to both, and only the selection of the override was written apart. Its Reference named no counterpart and no "None, this project's own design"; the program is this project's own design, modelled on the echo server.
- `docs/changes/261031/syscall.md`: "the kernel's translation is checked by `pinlines.test`" overstates. `pinlines.test` compares `platform::isa_irq_line()` with `apic_io_isa_to_global()`, the kernel's own table, and does not read the MADT; and it is a kernel-shell command that a person runs, which `boot_check.py` does not. No automated check compares the kernel's translation with the firmware's tables (`docs/repairs/261031/acpi-route.md`, item 1).
- `docs/changes/261031/pc-ps2.md`: "no boot can tell the two drivers apart" is too strong. Two drivers, one that uses the service's answer and one that uses the constant GSI 1, are told apart by a boot in which the service is made to answer another GSI for ISA 1; only the pairing with the unmutated service on QEMU's tables is indistinguishable. The mutants of the error path and of the log lines it lists are in the same class and are guarded by reading the code. A failed question leaves the keyboard dead until reboot, since the framework starts no exited component again; the driver depends on the service being up at its start and, now that the service follows the kernel's rules, on nothing stricter than the kernel is.
- `docs/changes/261031/init.md` and `acpi.md` call a Get of `ours.boot.Acpi` on a machine without tables "answered with why": it is BadHandle, as for every `ResourceServer` that holds no handle, and the line `init: started without ACPI tables` says why.

**Verification**
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 154 passed. The kernel says `userabi: 7 ACPI tables, 9384 bytes, handed to userboot; DSDT found, 0 left out`, the service `serving, from 7 ACPI tables`, pc-ps2 `ISA 1 is on GSI 1, edge, active high` and the keys, and `routing_checks: passed`.
- Fifteen mutations of the host tests and three booted on the rel image, each caught and each restored byte for byte: the first override winning, the bus required, a reserved trigger accepted; the size bound, the header length, the minimum length, the entries-must-fit, the alignment, the ordering and the size-beyond-bytes checks of the reader each removed in turn (the entries-must-fit one dies by a fault at the edge of the page); the export trusting a second walk, the skipped tables not counted and the DSDT not noted; the txid rule and the width of an enum in the codec; and, booted, the mapping of the trigger swapped, the mapping of the polarity swapped and the check's expectation for ISA 9 changed. Of the first run's mutants two were not equivalent to what they claimed (a `++skipped && false` still increments) and were rewritten.
- clang-tidy on the changed sources: no finding on a changed line.
- Not verified: the service's rule for two overrides, the bus and a malformed tail on a real machine (QEMU's table has none of the three, so the boot does not exercise them and the host tests do); the active-low path of the mapping on a real table (QEMU's overrides are all active high); any firmware but QEMU's q35 under edk2.

**Deliberately not done**
The five items of `docs/repairs/261031/acpi-route.md`. The service still answers one window for a segment and does not say that a segment may have several. The client still has no timeout. The route service's request paths (a malformed message, a full table, a failed re-arm) are covered by the codec's tests and by reading, not by a test of the server, because the server's class lives in the program's file. The independent oracle for ISA numbers that `interrupt_map(Isa)` gave is not replaced by anything but QEMU's table.

**How it was found**
By an independent review: four reviewers with a lens each (the kernel's hand-off, parsing untrusted bytes, the program and the codec, the wiring and the truth of the documents), a skeptic for each who tried to refute the findings and could reproduce them under ASan and UBSan, and two more on the keyboard and the ABI commits. The duplicate-override and the stale-mutation findings were reproduced by running the real functions on crafted tables; the guard-page way of seeing the reader's bound came out of the skeptic's note that the check could be seen failing only under a sanitizer.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "the service gives the same answer as the kernel" the property to keep, and not "the service is the safer reading of the table"?
- A check of a parser can be removed and the tests still pass. What did the cases have to do to prevent that, and why does a read past the end of a buffer go unnoticed in an ordinary test?
- The service answered the trigger and the polarity, and a swap of the two passed the whole boot. Why, and what does the check now hold the answers to?
