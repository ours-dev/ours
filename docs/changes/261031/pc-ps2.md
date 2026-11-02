# pc-ps2 changes

## pc-ps2 asks the ACPI route service which GSI the keyboard is on, and gives the kernel that, not an ISA number (2026-10-31)

**Files**
- `system/driver/pc-ps2/pc_ps2.cpp` — `keyboard_route()` asks `ours_acpi::isa_irq(ISA_KEYBOARD)` and logs the answer in one line; `take_line()` takes the GSI and calls `interrupt_map(irq, InterruptSourceKind::Gsi, gsi, &number)` where it called it with `Isa`; `main()` asks first and stops with the reason when the service cannot answer; the file's comment and the names of two failure lines are brought up to date.
- `system/driver/pc-ps2/BUILD.gn` — depends on `//system/lib/ours.acpi:client`; the manifest uses `ours.acpi.Route` besides the two resource protocols; the comments say what it uses.
- `kernel/products/image/dev/BUILD.gn` — `root.cm` offers `ours.acpi.Route` from `#acpi-route` to `#pc-ps2` as well as to `#routing_checks`, and the comments that list what each child gets say so.
- `.agents/tasks/acpi-route/PLAN.md`, `.agents/tasks/interrupt-system/PLAN.md`, `docs/repairs/261031/syscall.md` — the sentences that said pc-ps2 was not moved onto the service yet.

**Symptom**
Latent, cannot be triggered today: on QEMU's q35 the MADT has no override for ISA 1, so ISA 1 is GSI 1, and the old call `interrupt_map(irq, Isa, 1)` and the new one `interrupt_map(irq, Gsi, 1)` name the same line. What the old call cost is in the interface: `interrupt_map(resource, kind, source, out)` carries the x86 PC's ISA numbering in a syscall of a kernel meant to run on platforms that have none, and the only reason it does is that pc-ps2 knew no other number for its line (`docs/repairs/261031/syscall.md` item 1, interrupt-system D11). Until pc-ps2 stops using `Isa`, the kind cannot be removed.

**Cause**
Not a mistake in pc-ps2: it was the only way to name the line while nothing in user space could read the firmware's tables. Four details decide the new shape.

- **Why a driver must not name an ISA number to the kernel.** An ISA IRQ is a PC's name for one of the 16 legacy lines of the two 8259 interrupt controllers. A machine with an IO-APIC does not have those lines any more: each arrives on an IO-APIC pin, the chipset's wiring decides which, and the number of the pin is the GSI (global system interrupt), the platform's own number for it. A syscall that takes an ISA number makes the kernel keep the ISA-to-pin translation forever, and makes the interface itself x86-specific: an ARM or RISC-V platform has no ISA numbers to give it, and every driver that takes a legacy line adds one more caller of a kind that D11 wants gone. The kernel should offer the mechanism, "make an interrupt object on pin G of this platform", and leave the question "which pin is the keyboard's on this machine" to whoever reads the firmware's tables, which is now a user-space service.
- **What the MADT's interrupt source override is.** The MADT, the ACPI table that lists the interrupt controllers, can carry entries of the type *interrupt source override*: a bus (0 is ISA), an ISA IRQ number, the GSI it is really on, and flags for how it signals (polarity and trigger mode, each with a value that means "conforms to the bus", which for an ISA line is active high and edge triggered). With no override for a number, ISA N is GSI N. With one, it is not: q35 puts ISA 0, the timer, on GSI 2. The tables are allowed to do the same to ISA 1, and a driver that wrote "GSI 1" for the keyboard would be right on QEMU and name some other device's pin on such a machine; that is why the repair item ruled it out. What the old call did was ask the kernel to apply the overrides; the new one asks the service that applies them to the same tables.
- **Why a driver that asks the service is portable.** pc-ps2 states two facts and only one of them is the machine's. That the i8042 raises ISA IRQ 1 is the PC architecture's, as its ports 0x60 and 0x64 are, and it is written in the driver. Which pin that is on this machine is the firmware's, and the driver takes it from the one party that reads the firmware, so what changes between machines is the service's answer, never the driver. The kernel's side is one number, the platform's own, so the same `interrupt_map(resource, source, out)` serves a platform whose route service reads a device tree. The split is also one of power: the service holds the tables and no device resource, and the driver holds the device resources and no tables, so neither can do the other's harm.
- **Three numbers, three owners.** The ISA IRQ (1) is the device's. The GSI the service answers is the platform's pin. The number `interrupt_map` gives back is the kernel's own name for the line, assigned at boot and deliberately not the GSI (interrupt-system D3), and it is the one `interrupt_create` takes. The driver walks them in that order: ISA IRQ, route service, GSI, `interrupt_map`, kernel number, `interrupt_create`, interrupt object. On QEMU the first two are both 1 and the third is a different number by design, so passing a GSI where the kernel's number is due would not work by luck either.

Two choices of the driver's own.

- **The mode stays `ModeDefault`, though the service also says how the line signals.** The four modes `ModeEdgeLow`, `ModeEdgeHigh`, `ModeLevelLow` and `ModeLevelHigh` could express the route's trigger and polarity exactly, so this is a choice and not a limit. The kernel programmed every ISA pin from the same override table when it started (`parse_io_apic_isa_overrides()` and `init_io_apic()` in `kernel/platform/pc/interrupt.cpp`), so the pin already signals as the service says, and giving the mode would write the same values again and add a way to disagree with the table. The service's trigger and polarity are logged and used for nothing else. They would matter for a line the kernel does not program from the tables, such as a PCI function's, which only the AML decides; no driver of this image has one.
- **The question comes first in `main()`, before the I/O ports and the Irq resource.** A driver that cannot learn its line stops before it touches the controller, and holds no handle of the root while it waits for the service (a Get's duplicate is charged to the root until it is closed). It follows the failure paths the file already had: `pc-ps2: asking the route service for the keyboard's GSI: <status>` on the debug console, then exit 1, as `getting the IRQ resource` does. The statuses are those of the client (`ours_acpi/client.hpp`): NotFound for a route that leads nowhere or a component that has exited or a line the tables do not have, PeerClosed, InvArgs, or a channel call's own.

pc-ps2 is started before acpi-route (`root.cm` lists acpi-route last) and its question is still answered: in every rel log the order is `init: started pc-ps2`, `init: started acpi-route`, `acpi-route: serving`, then pc-ps2's GSI line. That init serves the opens after it has started its eager children, so a request waits in its channel until the service is up, is the reading of that order; it was not instrumented.

**Change**
The call that named the ISA number to the kernel, and the one that names the GSI:

```cpp
// before: take_line(irq), with
ours::interrupt_map(irq, ours::InterruptSourceKind::Isa, ISA_KEYBOARD, &number);

// after: take_line(irq, gsi), with the GSI from the service
auto const route = ours_acpi::isa_irq(ISA_KEYBOARD);
// ... on an error, report and exit 1; otherwise log it, then
ours::interrupt_map(irq, ours::InterruptSourceKind::Gsi, gsi, &number);
```

The line it logs, from the rel boot: `pc-ps2: ISA 1 is on GSI 1, edge, active high`. The manifest and the offer, the other two halves of the change:

```gn
protocol = [ "ours.resource.Irq", "ours.resource.Ioport", "ours.acpi.Route" ]   # pc-ps2.cm
to = [ "#routing_checks", "#pc-ps2" ]                                           # root.cm, the Route offer
```

Rejected: a fallback to `Isa` when the service fails (it would keep the kind alive and the failure unreported); giving the kernel the service's trigger and polarity as a mode (above); a retry or a wait in the driver for a service that is not up (the client already blocks until the reply or the epitaph).

**Reference**
None, this project's own design: interrupt-system D11 (`.agents/tasks/interrupt-system/DESIGN.md`) and the route service in `.agents/tasks/driver-manager/DESIGN.md`; the service is `docs/changes/261031/acpi-route.md`. For the ISA-to-GSI step Linux has `acpi_isa_irq_to_gsi()` and `mp_override_legacy_irq()` (`arch/x86/kernel/acpi/boot.c`), which its i8042 driver reaches through the IRQ it reads from the firmware's `_CRS` (`i8042_pnp_kbd_probe()`, `drivers/input/serio/i8042-acpipnpio.h`) instead of a constant; those names are from memory and were not re-read for this entry.

**Verification**
- `boot_check.py`, both images and the host tests, from the sources committed: build ok; dbg PASS (44 passed, 4 skipped); rel PASS (41 passed, 8 skipped, 6/6 required); host tests 154 passed (no test added). In the rel log: `acpi-route: serving, from 7 ACPI tables`, `pc-ps2: ISA 1 is on GSI 1, edge, active high`, `pc-ps2: ready`, `pc-ps2: key a`, `pc-ps2: key left-shift`, `pc-ps2: key A`, `routing_checks: passed`, and no FAILED line.
- Five mutations of the rel image (`boot_check.py --only rel --no-tests`), each restored byte for byte (checked by hash) and the final run above made from the restored sources. The required lines of 6 that came, in brackets:

| Mutation | What the log showed | Caught |
|---|---|---|
| (a) the service's GSI plus 1 | `pc-ps2: taking the keyboard's line: AlreadyBound` (GSI 2 is q35's pin for ISA 0, the timer, which the kernel's own handler is taken to hold; not instrumented) [3/6] | yes |
| (b) the offer to `#pc-ps2` removed from `root.cm` | `init: pc-ps2's use of "ours.acpi.Route" routes nowhere: NotFound`, then `pc-ps2: asking the route service for the keyboard's GSI: NotFound`, exit 1 [3/6] | yes |
| (c) it asks for ISA 12, the second port's line | `ISA 1 is on GSI 12` (the log line prints the constant, which the mutation did not change), `pc-ps2: ready`, and no key [4/6] | yes |
| (c2) it asks for ISA 2, the cascade | `pc-ps2: asking the route service for the keyboard's GSI: NotFound`, exit 1 [3/6] | yes |
| (d) it ignores the service's GSI and uses the constant 1 | identical to the unmutated boot [6/6, `key a` and `key A` present] | **no** |

- (d) is undetectable on this machine and is reported as such: QEMU's ISA 1 is GSI 1 with no override, so a driver that writes the constant gives the same line as one that asks. Nothing in this tree can make the MADT override ISA 1 (QEMU's tables are its own and no test machine builds one), so no boot can tell the two drivers apart. `acpi-route-test` covers the service's rule for overrides on crafted MADTs (ISA 0 onto GSI 2, the first of two overrides, level and low polarity), but none of its overrides puts ISA 1 on another GSI, and nothing tests pc-ps2 using the answer. The only guard on (d) is the reading of the code.
- clang-tidy (`prebuilt/clang/llvm-22/bin/clang-tidy -p out/bringup.x86 --quiet`) on `pc_ps2.cpp`: no findings; a copy with a planted misnamed function was flagged by the same invocation, so the silence is not a tool that did not run. `scripts/format_cpp.py` and `scripts/format_gn.py`: clean. `lint_docs.py --changed` on this record and the three documents edited: clean.
- Not verified: any MADT but QEMU's (a machine that overrides ISA 1 above all); a service that is up but never answers (pc-ps2 would wait for it, since the client's wait has no timeout); the dbg image, which prints no user-space output, so everything above of pc-ps2 is rel's alone.

**Deliberately not done**
`Isa` and `kind` stay in `interrupt_map`, and the checks of `routing_checks` (the cross-check of the service among them) and of the shell (`shell_tests.cpp`) still use `Isa`: removing them is the next commit, so that this one changes only who asks. The mode is not derived from the service's trigger and polarity (above); this is a design choice for the user to confirm or reverse, and the change to make it is a four-way mapping in `take_line()`. No timeout on the question, so a hung service holds the keyboard driver in its wait. The labels of the checks inside `take_line()` still say `ISA 1`, the keyboard's own name, where they describe the line. `platform::isa_irq_line()` in the kernel stays, as the repair item says.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- On QEMU the service answers GSI 1 for ISA 1, the same number the driver passed as an ISA number before. What does the change buy, and why can no boot on this machine show it?
- pc-ps2 receives the route's trigger and polarity and passes `ModeDefault`. What makes that correct for the keyboard's line, and for what kind of line would it stop being correct?
- Mutation (a) ends in `AlreadyBound` and not in a silent keyboard. Whose line is GSI 2 on q35, and why does a second interrupt object on it fail?
