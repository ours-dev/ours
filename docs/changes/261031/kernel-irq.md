# kernel-irq changes

## A linear domain gives every line a virq, and a line is named by it, not by its GSI (2026-10-31)

**Files**
- `kernel/include/ours/irq/domain.hpp`, `kernel/kernel/irq/domain.cpp`, `kernel/kernel/irq/BUILD.gn` — `irq::Domain`, the empty scaffold given its content: hardware number to virq and back.
- `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp` — `DescTable` owns a `Domain`; `init()` builds it and makes each line with its virq; the control path turns a virq into the hardware number first; `handle_irq()` takes the hardware number.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp`, `kernel/include/ours/irq/types.hpp` — `irq::virq_of_hwirq()`, `irq::hwirq_of_virq()`, `INVALID_H_IRQ_NUM`; `handle_irq_generic()` takes the hardware number.
- `kernel/platform/pc/interrupt.cpp`, `kernel/platform/include/ours/platform/isa.hpp` — `platform::remap_isa_irq()` answers a virq.
- `kernel/arch/x86/kernel/time/time.cpp`, `kernel/arch/x86/kernel/time/hpet.cpp` — the PIT's line is named by `remap_isa_irq(ISA_IRQ_PIT)`; the HPET channel's clock event device stores the virq of its routing pin, not the pin.
- `kernel/commands/pin_lines_test.cpp`, `kernel/commands/irq_test.cpp`, `kernel/commands/interrupt_line_test.cpp` — the tests name lines by virq; `pinlines.test` gains the domain checks and an ISA oracle.
- `kernel/commands/irq_domain_test.cpp`, `kernel/commands/BUILD.gn` — new kshell test `irqdomain.test`: the domain on layouts this machine does not have, and on lists it must refuse.
- `system/app/shell/shell_tests.cpp` — the user-space check that a GSI taken for a line names none.
- `system/public/ours/interrupt.hpp`, `system/public/ours/syscalls.gni`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp`, `kernel/platform/include/ours/platform/isa.hpp` — the documentation of `line`.
- `.agents/skills/ours-kernel-design/references/task.md` — the decision, with its reason.
- `.agents/tasks/interrupt-system/DESIGN.md` — the open question "where do virqs start" is answered.

**Symptom**
Every user program that holds an interrupt names it by the pin's GSI: `interrupt_create(root, line)` takes the GSI as `line`. A GSI is an x86 and ACPI number. ARM has GIC numbers, an MSI has no GSI at all, and a driver that computes with the number is tied to the PC. The user decided on 2026-10-31 that user programs must not handle GSIs (interrupt-system D1). Nothing in the tree showed whether a program depended on the two being equal, because they always were: `VIrqNum` and `HIrqNum` are both `u32`, the line table was indexed by one number, and `Data` carried the same value twice.

**Cause**
Not a bug: the first version had one number space and said so (`.agents/tasks/irq-genirq/DESIGN.md` D4 deferred `irq_domain` to MSI). The objects:

- A hardware number (hwirq), the controller's number for a pin; on x86 the GSI. The platform programs the pin with the vector `PlatformIrqMin + hwirq`, and the interrupt entry subtracts the base to get it back. Chips find their pin from it (`data.hirqnum`).
- A kernel number (virq), the name every other part uses for the line: an `Action` records it, an interrupt object holds it as `m_line`, `interrupt_create` takes it.
- The line table, an array read without a lock in interrupt context.

Giving the two different values means each caller has to be on the right side. The interrupt entry has only the hardware number, so it must index the table by it; everything else has a virq, so it must be translated before it can index. Because both are `u32`, a mix-up compiles, and the way to find every site is to make the values differ so a wrong one fails at run time.

**Change**
`irq::Domain` keeps two arrays, built once before any line is unmasked and only read after, so the lookups take no lock:

```cpp
    /// virq of each hardware number, INVALID_V_IRQ_NUM where none; by hwirq.
    ustl::views::Span<VIrqNum> m_virqs;
    /// hardware number of each virq; by virq less m_first.
    ustl::views::Span<HIrqNum> m_hwirqs;
```

The virqs are consecutive, in the order of the hardware numbers, from the first multiple of `VIRQ_ALIGN` (32) past the highest one. On q35's 24 pins that is 32 to 55. Past the highest is the point: every virq is above every hwirq of the machine, so a program that takes a GSI for a virq finds no line (`NoResource`) instead of another pin's. A start of 0 would be the identity on a machine whose GSIs begin at 0, and `0x100` would leave 256 empty slots in a table indexed by virq; the table is not indexed by virq, but the rule that no virq equals an hwirq is the one worth keeping.

`DescTable` stays indexed by hardware number, so the interrupt path is the lookup it was:

```cpp
auto DescTable::handle_irq(HIrqNum const hirqnum, Eoi const eoi) -> ustl::Option<IrqReturn> {
    auto *const desc = hwirq_to_desc(hirqnum);
```

and the control path adds one translation in front:

```cpp
auto DescTable::irqnum_to_desc(VIrqNum const virqnum) -> Desc * {
    return hwirq_to_desc(m_domain.hwirq_of(virqnum));
}
```

`platform::remap_isa_irq()` asks the irq layer which line was made for the ISA IRQ's GSI. `RemapIrq` therefore keeps working unchanged for pc-ps2 and the user-space checks, which is why they needed no edit. The test that raises a line's vector by IPI now builds it from `irq::hwirq_of_virq()`.

Rejected: indexing the table by virq and translating on the interrupt path (a second dependent load in every interrupt, for nothing the control path needs); a virq start at a fixed `0x100` (above); a Linux-style sparse descriptor tree or radix-tree domain (the design's Alternatives: every pin is known at boot, so there is nothing to create at run time, and a changing structure would need RCU or a lock in the interrupt path); making `VIrqNum` and `HIrqNum` distinct types (it would let the compiler find every site, but touches syscalls, logs and every test, and is its own change).

**Reference**
None, this project's own design: `.agents/tasks/interrupt-system/DESIGN.md` D1 and D3, `PLAN.md` step I3. The linear domain is the smallest form of Linux's `irq_domain` (`irq_domain_add_linear()`); the table indexed by the hardware number on the interrupt path is the reason it needs no radix tree.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- Build: ok.
- `pinlines.test`: ok, 28 checks (14 before I3). 24 pins map to virqs from 32; each virq maps back to its GSI and is not the GSI; the virqs are consecutive in the order of the GSIs from the first multiple of 32 past the highest, checked against that rule written out and not against the domain; the ends of both tables answer INVALID, the interrupt path included; each ISA IRQ names the line made for the pin the MADT sends it to, asked of `apic_io_isa_to_global()`; a GSI used as a virq names no line; an object takes the last pin by its virq and a second answers `AlreadyBound`.
- `irqdomain.test` (new): ok, 39 checks. The domain built from one number, from numbers with holes listed out of order, from a highest number at 32 (the first virq moves to 64), from two runs of pins listed last run first, and from this machine's layout; each is checked for every number around and past the list, in both directions. Refused: an empty list, `INVALID_H_IRQ_NUM`, a list the virq space cannot hold (`InvArgs`), a number twice (`AlreadyExists`), which keeps nothing, so the same domain then takes a good list; a second build (`BadState`) keeps the map.
- The other interrupt tests have the same number of checks as before: `interrupt.test` 32, `interruptline.test` 58, `irq.test` 72, `resource.test` 60, all ok.
- `boot_check.py`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required (the keyboard's keys arrive through `RemapIrq`, the virq path); host tests 151 passed, 0 failed. The same with `DISK=` set. The user-space group `shell: interrupt checks` counts as one, and now includes the GSI-not-remapped check.
- Mutation 1, the domain made the identity (`first = 0`; on q35 the GSIs are 0 to 23 so every virq equals its GSI): `pinlines.test` fails `3 of 23 checks` (`is not the GSI`, `every virq is past every GSI`, `a GSI used as a virq names no line`), while the other 20 and the other four interrupt tests pass: an identity map is a consistent one, which is why those checks exist.
- Mutation 2, the interrupt path looking the line up by virq instead of hardware number: the machine does not boot. The console goes silent and the script's NMI ends it; the `rel` image fails with `0/6 required`.
- Five more mutations, the ones the review below said every check would let through, each applied alone, built clean and run, then restored (`cmp` against the saved copy): the first virq rounded up from the highest hardware number instead of one past it, which is the same on this machine (`irqdomain.test` fails, 2 of 39); the bound of `Domain::virq_of()` off by one (`irqdomain.test` fails); the bound of the line table off by one on the interrupt path (`pinlines.test` does not finish: the read past the table); `RemapIrq` honoured whether asked for or not (dbg: 43 passed, 1 failed, `shell: interrupt checks FAILED`); a wrong but valid line for ISA 3 and up (`pinlines.test` fails 1 of 28 and nothing else does). All five are caught.
- `scripts/format_cpp.py` and `scripts/format_gn.py` clean on every changed file. clang-tidy 23.1.0-rc3 against the build's `compile_commands.json`: no diagnostics in any changed file or in the lines changed in `shell_tests.cpp`, other than two on `hpet.cpp:241-242` (`id_`, `regs_mmio_`), which are not lines of this change.

Not measured: more than one IO-APIC (the domain's start would move past 32), a machine whose GSIs do not begin at 0, ARM and RISC-V (their platform code was not built; it was read, not compiled), real hardware, QEMU 11.1.1.

**Deliberately not done**
- `interrupt_map`: a program still gets a virq only through `RemapIrq`, or by being handed a number. The call is step I4.
- `RemapIrq` stays until step I4b (interrupt-system D5).
- A way for a program to learn the line of a pin that is not an ISA device's. Until step I4 a user program can name no line past ISA 15: `RemapIrq` is the only way yet to get a number, and a GSI names no line. Nothing in the tree needs one in between.
- The virq to descriptor lookup is not dynamic. It becomes so with MSI (P3); `DescTable::irqnum_to_desc()` and `find_line()` are its only readers.
- Distinct `VIrqNum` and `HIrqNum` types: see Change.
- A stable virq across boots or machines: the contract is that a program does not compute one.

**How it was found**
The change was written, verified and mutated by the author, and then read by four independent reviewers with different lenses (the interrupt path and concurrency; numbers from outside and arithmetic; completeness of the migration; strength of the tests), each of whose findings was given to a skeptic told to refute it. They reported 11 findings, all low, and the skeptics refuted none. Handled:

- The HPET channel stored its IO-APIC routing, a hardware number, in the clock event device's virq slot (`hpet.cpp`), which `tick_device.cpp` hands to `irq::set_affinity()`. Found by three of the four reviewers, and missed by the author's own search, which looked for calls of the irq layer and not for a number stored in a field of the right type. It did nothing today only because `set_affinity()` is a stub. Fixed: it stores `irq::virq_of_hwirq()` of the pin.
- Two comments (`isa.hpp`, `interrupt.cpp`) still said the GSI names the line, and the public documentation promised that a program "gets" a virq when nothing hands one out yet. Fixed, and the documentation now says that `RemapIrq` is the only way yet.
- The new test's order check assumed the IO-APICs are listed in GSI order, and would have failed on a correct domain of a machine whose MADT lists them otherwise. Rewritten as a rank: a pin's virq is the first plus the number of pins below it.
- Five ways the checks let a wrong change through (the mutations above): a domain tested on one layout only, the ends of the tables, `RemapIrq` never shown to be needed, and ISA lines named with the function under test. Fixed by `irqdomain.test`, the end checks, the user-space check, and the ISA oracle.

The reviewers read the diff and the tree; none built or ran anything. The author's first search for places that mix the two numbers is the method the review corrected: make the values differ so a wrong one fails (mutation 2 shows it does), and then search for every number that is stored, not only every call.

**The user's prediction**
The user was asked on 2026-10-31 which of the two lookups, virq to descriptor or hwirq to virq, fails first when MSI arrives, and did not answer. Their decision that the virq starts at 32 (a number past the largest GSI) is the one implemented.

**Three questions you should be able to answer**
1. The interrupt entry has the hardware number and a user program has a virq. Which of them indexes the line table, and why does that keep the interrupt path as cheap as it was?
2. Why must every virq be greater than every hardware number, and what would an identity domain on q35 have hidden from every test that existed before `pinlines.test` gained its three checks?
3. `HIrqNum` and `VIrqNum` are both `u32`. What did you do instead of the compiler to find every place that mixed them up, which mutation shows the interrupt path is guarded, and what kind of place did the author's own search miss that three reviewers found?

## The PCI pin the checks take is GSI 16, not the one a virtio-blk is on (2026-10-31)

**Files**
- `kernel/commands/pin_lines_test.cpp` — the object check takes GSI 16, the first pin past the ISA range, and not the last pin; `last_pin()` is gone.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the PCI pin `interrupt_map` is asked for is GSI 16.

**Symptom**
Latent, cannot be triggered today. Both checks took GSI 23 as "a PCI pin", chosen as the last pin the machine has. With `DISK=` set a virtio-blk is on GSI 23, so a driver that held its interrupt object while `routing_checks` ran (`rel` runs it at every boot) would make the check's `interrupt_create` answer `AlreadyBound`, and the image would report a failure that is no fault of the kernel.

**Cause**
The pin was picked for being the last one, with no thought for which PCI function the machine wires to it. On QEMU's q35 a function on the root bus at slot S, interrupt pin P (A is 0), is wired to a pin like this:

- The ICH9 chipset sends INT[A-D] of slot S to PIRQ `(S + P) % 4 + 4`, that is PIRQ E to H (`hw/isa/lpc_ich9.c`, `ich9_cc_init()`), and in APIC mode PIRQ n is GSI `16 + n` (`ich9_pirq_to_gsi()`): GSI 20 + ((S + P) & 3). Slots 25 to 29 and 31 go to PIRQ A to D instead: GSI 16 + P.
- The firmware hands the operating system the same table as ACPI's `_PRT` (`hw/i386/acpi-build.c`, `build_q35_routing_table()`).

The first function QEMU adds is at slot 3, pin A: GSI 20 + 3 = 23.

**Change**
Both checks take GSI 16 (PIRQ A), which no function at slots 0 to 24 uses.

**Reference**
None, this project's own tooling. The rule is read from QEMU 8.2.2's source (the Ubuntu source package `qemu_8.2.2+ds.orig.tar.xz`, checked against its published SHA-256).

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- The rule was checked against the table the firmware gave the guest, and not only against the source: the guest was booted with `DISK=`, its memory dumped through the QEMU monitor (`pmemsave`), and the routing table for APIC mode found in the dump and decoded. Slot 3, pin A to D: GSI 23, 20, 21, 22. Slot 1: 21, 22, 23, 20. Slot 4 and slot 24: 20, 21, 22, 23. Slots 25, 29, 31: 16, 17, 18, 19. All agree with the rule.
- `pinlines.test`: ok, 28 checks. `boot_check.py`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required; host tests 151 passed, 0 failed; the same with `DISK=` set.

Not measured: that an interrupt from the virtio-blk arrives on GSI 23 (the table says where it is wired; nothing here made the device raise one); QEMU 11.1.1 and its firmware.

**Deliberately not done**
- A device driver or a device interrupt test in the tree: the user decided on 2026-10-31 that the virtio-blk driver on the other side is the first test of a device's interrupt.

**How it was found**
From the rule itself. Working out which GSI a PCI function gets for the hand-over (the Interrupt Line register reads 11, which is not a GSI) showed that the pin the tests had picked is the one the first function lands on.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A PCI function at slot 3, pin A, on q35: which GSI, and by which two steps from the slot and the pin?
2. Why is the Interrupt Line register no help, and where does the operating system read the routing from instead?
3. Why does it matter that a check takes a pin no function is wired to, when the check closes its object again at once?
