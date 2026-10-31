# syscall changes

## `interrupt_map` gives the number of the line for a source the firmware names (2026-10-31)

**Files**
- `system/public/ours/syscalls.gni` — the new entry `interrupt_map(resource, kind, source, out)`, with its documentation.
- `system/public/ours/interrupt.hpp` — `InterruptSourceKind`, `Isa = 1` and `Gsi = 2`.
- `kernel/lib/syscall/src/interrupt.cpp` — `syscall::interrupt_map()`.
- `kernel/object/resource_dispatcher.cpp`, `kernel/object/include/ours/object/resource_dispatcher.hpp` — `validate_root_resource()`, next to `validate_ranged_resource()`.
- `kernel/platform/include/ours/platform/isa.hpp`, `kernel/platform/pc/interrupt.cpp`, `kernel/platform/arm/isa.cpp`, `kernel/platform/riscv/isa.cpp` — `platform::isa_irq_line()`; `remap_isa_irq()` asks it.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/commands/resource_test.cpp`, `kernel/commands/pin_lines_test.cpp`, `system/app/shell/shell_tests.cpp` — the checks.
- `kernel/object/interrupt_event_dispatcher.cpp` — `RemapIrq` on an ISA IRQ with no line answers `NoResource` (see Change, the review).

**Symptom**
After the previous change a line is named by the kernel's own number, never the pin's GSI, and `RemapIrq` is the only way a program learns one, for an ISA device. A PCI function's INTx is on a pin past the ISA range (GSI 16 to 23 on q35), which has a line since the pins were programmed, and no program could name it. The component that reads the firmware's tables and knows a device's GSI had no way to turn it into the number `interrupt_create` takes.

**Cause**
Not a mistake: the second half of interrupt-system D1 and D2. The user decided that programs do not handle GSIs but use the kernel's number, which needs one place where a GSI is turned into it: a call for the one trusted component that reads the firmware (ACPI), so that a device driver is handed an interrupt object and holds no number at all. The objects:

- A source, what the firmware names: an ISA IRQ or a GSI. Another architecture adds its own kind.
- A line, named by a virq. Which line a source has is known only to the kernel: the platform made the lines from the MADT and the irq layer numbered them.
- The interrupt root, the Irq resource that covers every line.

**Change**
The call checks, in this order, because each check names the argument it is about (as `interrupt_create` checks its options before the resource): the kind is one the kernel knows (`InvArgs`), the source is not `0xFFFFFFFF` (`InvArgs`), the resource is the interrupt root (`BadHandle`, `BadType`, `AccessDenied`), the source has a line (`NoResource`), and the number is written out. Nothing is taken: the line stays free until `interrupt_create`, so there is nothing to give back if the write fails.

```cpp
    if (auto const status = object::validate_root_resource(resource, RsrcKind::Irq);
        status != Status::Ok) {
        return status;
    }
    ...
        case InterruptSourceKind::Isa:
            virq = platform::isa_irq_line(source);
            break;
        case InterruptSourceKind::Gsi:
            virq = irq::virq_of_hwirq(source);
            break;
```

The resource must be the root, and not a resource that covers the line: interrupt-system D4. A range is made of the kernel's numbers (virqs) and the call takes the firmware's, so a narrowed resource cannot say which sources it covers, and "a resource covering the hwirq" would compare numbers of two spaces. `validate_root_resource()` is the object layer's answer, in the shape of `validate_ranged_resource()`, with no right required of the handle: holding it is the capability.

`platform::isa_irq_line()` is the one place an ISA IRQ becomes a line. It leaves out the cascade, ISA 2: the platform does not program it, and on q35 its GSI is the timer's (ISA 0 is on GSI 2), so naming it would hand out the timer's line under a second name. `remap_isa_irq()` now asks it, so `RemapIrq` gives no line for ISA 2 either; nothing used it. On ARM and RISC-V, which have no ISA interrupts, it answers none.

The kinds start at 1, so an argument left zero is refused. Rejected: a resource covering the source (above); one call per kind; returning the hardware number back, which nothing needs.

**Reference**
None, this project's own design: `.agents/tasks/interrupt-system/DESIGN.md`, "The call that turns hardware into a virq", and D2 and D4. Zircon has no counterpart; its `zx_interrupt_create(src_num)` takes the GSI.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- `routing_checks` on the rel image (it holds the interrupt root and the I/O port root): ISA 0 and GSI 2 give one number, and it is not 0 or 2; an interrupt on it answers `AlreadyBound`, the kernel's timer; ISA 3 and GSI 3 give one number, which `interrupt_create` takes without `RemapIrq`; GSI 23, a PCI pin, gives its own, which an object takes level-triggered; kind 0 and kind 99 are `InvArgs`, ahead of an invalid handle; `0xFFFFFFFF` is `InvArgs` for both kinds; ISA 16, the cascade and GSI 200 are `NoResource`; no handle is `BadHandle`; the I/O port root is `BadType`; a refused call writes nothing.
- `resource.test`: ok, 69 checks (60 before). The interrupt root passes `validate_root_resource()`; one line, and every line but the last, are `AccessDenied`; the I/O port root and the interrupt root are `BadType` for each other's kind. `AccessDenied` is shown here because a program has no call that makes a narrower resource.
- `pinlines.test`: ok, 29 checks (28 before): the ISA lines are checked against the MADT, the cascade names none, `isa_irq_line()` and `remap_isa_irq()` agree, and a number past the ISA IRQs is no ISA IRQ.
- The other interrupt tests have the same number of checks as before: `irqdomain.test` 39, `interrupt.test` 32, `interruptline.test` 58, `irq.test` 72, all ok.
- `boot_check.py`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required; host tests 151 passed, 0 failed.
- After the review, the checks it asked for: `routing_checks` also tries `Isa` at `0x100` and `0x103`, `Gsi` at 0, an invalid source with no handle (`InvArgs`) and a wrong kind of resource with a source that has no line (`BadType`); the user shell (dbg) tries `interrupt_map` with an `out` that faults (`InvArgs`) and `RemapIrq` on ISA 2 (`NoResource`); `resource.test` a range over every line (`AccessDenied`, though it grants every line); `pinlines.test` the wrapped ISA numbers. Four more mutations, each applied alone, built clean and run, then restored: the ISA bound tested on 8 bits (`pinlines.test` fails 1 of 29); GSI 0 answered through the ISA overrides (rel fails: `GSI 0 is the line ISA 0 was moved to`); the resource checked before the source (rel fails: `BadHandle (expected InvArgs)`); `RemapIrq` on the cascade back to `InvArgs` (dbg fails: `shell: interrupt checks`). All caught. `resource.test` 72 checks, `pinlines.test` 29; boot_check unchanged (dbg 44/4/0, rel 41/8/0, 6/6, host 151/0), with and without `DISK=`.
- Three mutations, each applied alone, built clean and run, then restored and compared with the saved copy: the root test of `validate_root_resource()` turned off (`resource.test` fails 2 of 69); the Isa kind answered as if it were a GSI (rel fails, `5/6 required`: ISA 0 and GSI 2 differ, and the timer's line is taken); `interrupt_map` that never looks at the resource (rel fails: a missing handle answers `Ok`, and the output is written though the call refused). All caught.
- `scripts/format_cpp.py` and `scripts/format_gn.py` clean on every changed file. The ARM and RISC-V stubs were compiled as syntax only, with their own target triples (`-fsyntax-only`, `-Werror`, `-Wmissing-declarations`): clean.

Not measured: more than one IO-APIC; a machine where an ISA IRQ is moved to a pin with no line; ARM and RISC-V beyond the syntax check; real hardware; QEMU 11.1.1. The independent read of a call that takes a user-chosen number, which the plan asks for before such a call is committed, was not done before the commit `4a7a90d`: the user asked for it to be committed and pushed at once. It was done on that commit, and its findings are in "How it was found" and were fixed in the commit that follows it.

**Deliberately not done**
- `RemapIrq` stays until step I4b (interrupt-system D5).
- Narrowing: a driver is handed an interrupt object and not a resource; narrowing is P2.
- MSI as a source kind (P3), and other architectures' kinds.
- The ACPI `_PRT`: the caller still has to know the device's GSI (open question in the design, to be decided before step I5).

**How it was found**
After the commit `4a7a90d`, and at the user's request that it be committed and pushed first, three independent read-only reviewers (the call and its privilege check, the platform seam and the other architectures, the strength of the checks) read it, and a skeptic tried to refute each of their findings. 9 findings, all low; 8 stood. No privilege or isolation problem and no memory-safety problem was found. Handled in the commit after `4a7a90d`:

- `RemapIrq` on ISA 2 answered `InvArgs` where `interrupt_map(Isa, 2)` answers `NoResource`: the ISA IRQ with no line became the invalid number, which the irq layer refuses. Fixed in the dispatcher: an ISA IRQ with no line answers as any number with no line, while a `line` of `0xFFFFFFFF` keeps its `InvArgs`.
- The public documentation still called `RemapIrq` the only way to learn a line's number, did not say that ISA 2 has none, and said the resource must "cover all" lines when what is checked is that it is the root: a range over every line, which `ResourceDispatcher::create` accepts, is `AccessDenied`. All three corrected; the last is now pinned by `resource.test` (72 checks, 3 more).
- Four ways the checks let a wrong change through, each now caught (below): a source checked after the handle instead of before; an ISA number cast to 8 bits before its bound (`0x100` and `0x103` would be ISA 0 and ISA 3); `Gsi` routed through the ISA overrides, which only differ at GSI 0 on q35; an `out` pointer that faults, never tried for this call.
- One finding was refuted, correctly: that no check ties `interrupt_map` to the root-only gate, since replacing it with a range check passes every check. It is true, and cannot be closed yet: a program holds only roots, so for every resource it can hold the two tests agree. It will be checkable once a call that narrows a resource exists (P2).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `interrupt_map` ask for the interrupt root and not for a resource that covers the source, and why can the kernel not tell which sources a narrowed resource covers?
2. ISA 0 and GSI 2 are one line on q35. Why does the call leave out ISA 2, and what would a driver that asked for it have been given?
3. A refused call must write nothing. Which of the mutations shows the checks cover that, and what in the order of the checks makes the write the last thing that can fail?
4. A range over every interrupt line grants every line. Why is it still not enough for `interrupt_map`, and why can no test through the system call tell the root from such a range yet?

## `RemapIrq` is retired: a program asks `interrupt_map` for an ISA device's line (2026-10-31)

**Files**
- `system/public/ours/interrupt.hpp` — the enumerator `RemapIrq` is gone; the bit `0x1` is documented as retired and reserved.
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/interrupt.cpp` — `interrupt_create`'s documentation; `0x1` is no longer a known option.
- `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — the options are a mode and nothing else; the remapping and its `NoResource` special case are gone.
- `kernel/platform/include/ours/platform/isa.hpp`, `kernel/platform/pc/interrupt.cpp`, `kernel/platform/arm/isa.cpp`, `kernel/platform/riscv/isa.cpp` — `platform::remap_isa_irq()` is removed; `isa_irq_line()` is the one translation.
- `kernel/arch/x86/kernel/time/time.cpp`, `kernel/arch/x86/kernel/time/hpet.cpp` — the PIT's line comes from `isa_irq_line()`.
- `system/driver/pc-ps2/pc_ps2.cpp`, `system/app/shell/shell_tests.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/commands/interrupt_line_test.cpp`, `kernel/commands/irq_test.cpp`, `kernel/commands/pin_lines_test.cpp` — ask `interrupt_map` (user space) or `isa_irq_line()` (kernel) for a line's number, and check the retired bit.

**Symptom**
`interrupt_create` took its `line` in two meanings, chosen by an option bit: the kernel's number for the line, or, with `RemapIrq`, an ISA IRQ that the kernel turned into one. It was the one place a number that was not a line's number went into the call, and the review of the previous change found that the two meanings gave different answers for the same ISA 2 (`InvArgs` and `NoResource`).

**Cause**
Not a mistake: `RemapIrq` was the way across while no call returned a line's number, and `interrupt-system` D5 decided to retire it once `interrupt_map` existed (the user chose "remove now", read as a step of its own after `interrupt_map`). Two meanings for one argument is also what let a program hold a GSI where a line was due: an option bit, easy to leave set, decided which space the number was in.

**Change**
`interrupt_create(resource, line, options)` has one meaning for `line`. A driver, or a test, that knows a device by its ISA IRQ asks `interrupt_map(root, Isa, irq, &line)` and creates with the number it was given; pc-ps2 does exactly that:

```cpp
    auto number = u32{0};
    if (auto const status =
            ours::interrupt_map(irq, ours::InterruptSourceKind::Isa, ISA_KEYBOARD, &number);
        status != Status::Ok) {
        report("interrupt_map(ISA 1)", status);
        return ustl::err(status);
    }
    ... ours::interrupt_create(irq, number, OPTIONS, &first)
```

The bit `0x1` is not reused: option values are ABI, and a program built against the old header that still sets it must be refused and not find another meaning there. The syscall layer refuses it with every option it does not know (`InvArgs`, before the resource is looked at), and the object layer, which a kernel caller reaches without the syscall, refuses all but a mode. The enumerator is removed so that nothing in the tree can set it by name; the comment in `interrupt.hpp` keeps the number reserved.

`platform::remap_isa_irq()` is deleted, and not kept as "the Isa kind's translation" as the plan first said: `isa_irq_line()`, added with `interrupt_map`, is that translation, and `remap_isa_irq()` only added the rule that a number past the ISA IRQs comes back unchanged, which served `RemapIrq` alone. The callers in the kernel (the PIT and HPET code, the tests) ask `isa_irq_line()`.

**Reference**
None, this project's own design: `.agents/tasks/interrupt-system/DESIGN.md` D5 and `PLAN.md` step I4b.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- `boot_check.py`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required, with `pc-ps2: ready`, `pc-ps2: key a` and `pc-ps2: key A` (the keyboard's interrupt now reaches pc-ps2 through `interrupt_map`); host tests 151 passed, 0 failed. The same with `DISK=` set.
- `interruptline.test`: ok, 59 checks (58 before): the object layer refuses `0x1` (`InvArgs`), ahead of the line, and a number no line has still answers `NoResource`. `pinlines.test`: ok, 28 checks (29 before: the comparison of `remap_isa_irq()` with `isa_irq_line()` has nothing left to compare). `resource.test` 72, `irqdomain.test` 39, `interrupt.test` 32, `irq.test` 72, all ok.
- The user shell (dbg) takes its two lines from `interrupt_map` and runs the same group of checks as before, now with the retired bit refused (`InvArgs`), the cascade mapping to nothing, and a create with `Virtual` and a mode refused.
- Three mutations, each applied alone, built clean and run, then restored: pc-ps2 naming the keyboard's line by its ISA number (rel fails, `3/6 required`: the keyboard never arrives); the object layer letting `0x1` through (`interruptline.test` fails, 1 of 59); both layers letting it through (dbg fails: `shell: interrupt checks`). All caught. The syscall layer's refusal alone is not caught by anything: with only that layer open, the object layer refuses `0x1` itself, so each layer is shown by a different test, the object layer's by `interruptline.test` and the pair by the user shell.
- `scripts/format_cpp.py` and `scripts/format_gn.py` clean on every changed file; the ARM and RISC-V platform files compile as syntax only (they only lost a function).

Not measured: real hardware, QEMU 11.1.1; a program built against the old header (there is none in the tree).

**Deliberately not done**
- A compatibility path for `RemapIrq`: nothing outside the tree uses the ABI yet, and a path would keep the two meanings.
- A named constant for the reserved bit in the public header: a name invites a use.
- The independent review of this step: it removes a path and adds no call. The previous step's review (three reviewers, a skeptic each) covered `interrupt_map` and the platform function this one relies on.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the bit `0x1` stay reserved and refused, and not simply become available for a new option?
2. `remap_isa_irq()` is deleted although the plan first said it would become the Isa kind's translation. What replaced it, and what did it do that nothing needs now?
3. Which mutation shows that pc-ps2 really gets its line from `interrupt_map`, and what does the rel image report when it does not?

## `interrupt_map` names the line by the platform's own number: `Isa` and `kind` are gone (2026-10-31)

**Files**
- `system/public/ours/syscalls.gni` — `interrupt_map(resource, source, out)`: the `kind` argument is removed and the documentation rewritten (what `source` is, the errors that remain); the brief and the text of `interrupt_create` say "a pin the platform names".
- `system/public/ours/interrupt.hpp` — `InterruptSourceKind` is deleted; the comments of `InterruptOptions` no longer say that an ISA IRQ is something `interrupt_map` takes.
- `kernel/lib/syscall/src/interrupt.cpp` — `syscall::interrupt_map()` loses the kind check and the switch and looks the source up with `irq::virq_of_hwirq()` alone; the include of `ours/platform/isa.hpp` goes with the only call that needed it.
- `kernel/platform/include/ours/platform/isa.hpp`, `kernel/commands/pin_lines_test.cpp` — comments only: `platform::isa_irq_line()` stays, for the timer code and the tests, and is no longer "what the Isa kind asks for".
- `system/driver/pc-ps2/pc_ps2.cpp` — the call has three arguments.
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_interrupt_map()` loses the unknown-kind and `Isa` cases and asks about GSIs; `ISA_FREE` becomes `GSI_FREE`; `check_acpi_route()` is rewritten without the kernel's ISA translation (see Cause, "What was given up").
- `system/app/shell/shell_tests.cpp` — `check_interrupt_lines()` asks for GSI 2 and GSI 3 where it asked for ISA 0 and ISA 3, and the cascade case becomes a source no pin has plus `0xFFFFFFFF`.
- `.agents/tasks/interrupt-system/DESIGN.md`, `.agents/tasks/interrupt-system/PLAN.md`, `.agents/tasks/driver-capabilities/RESULT.md`, `.agents/tasks/acpi-route/PLAN.md`, `.agents/tasks/driver-manager/DESIGN.md`, `docs/repairs/261031/x86-apic.md`, `system/lib/ours.resource/README.md` — the call is described as it now is; D11 is marked done; the hand-over to the virtio-blk driver's author gives `interrupt_map(irq_root, gsi, &line)`.
- `docs/repairs/261031/syscall.md` — deleted: its one item is this change.

**Symptom**
Latent, cannot be triggered today: every caller already passed a source that means the same on its own platform. What was wrong is the interface. `interrupt_map(resource, kind, source, out)` carried `InterruptSourceKind { Isa = 1, Gsi = 2 }`, so the system call of a kernel meant to run on more than one architecture took a number of the x86 PC's legacy numbering, and its callers (the virtio-blk driver of `.agents/tasks/driver-capabilities/` was the next one) were handed that choice. The ABI of a young call gets dearer with each caller, so the time to change it was before the second one.

**Cause**
Not a mistake of the call's author: `Isa` was the only way pc-ps2 could name its line while nothing in user space read the firmware's tables (`docs/repairs/261031/syscall.md`, now deleted). It stopped being necessary the moment the route service existed and pc-ps2 asked it (`docs/changes/261031/pc-ps2.md`). Why an ABI that names ISA numbers is wrong, and what replaces it:

- **An ISA IRQ is a device's name, not a pin.** ISA IRQ 0 to 15 are the lines of the two 8259 interrupt controllers of the PC/AT; a legacy device (the PIT, the keyboard, a serial port) is known by the number of the line it was wired to in 1984. A machine with an IO-APIC routes each of them to a pin of its own, and the firmware says which: the MADT's interrupt source overrides. With none, ISA N is pin N; with one, it is not (q35 puts ISA 0, the timer, on GSI 2, so ISA 0 and GSI 2 are one line). A system call that takes an ISA number therefore has two names for one line, and it makes the kernel keep the PC's ISA-to-pin translation reachable by every program on every platform: ARM and RISC-V have no ISA numbers to give it.
- **The pin is the platform's number, and a platform has one number space.** The kernel is built for one platform, which numbers the pins of its root interrupt controller one way. On an ACPI platform that number is the GSI, the global system interrupt (an IO-APIC pin on a PC; with several IO-APICs the GSI space is their pins laid end to end by each one's GSI base). On a platform that follows a device tree it is the root controller's own number for the source: for an Arm GIC, the interrupt ID that a device-tree `interrupts` entry turns into (an SPI's number plus the 32 ids of the private interrupts); for a RISC-V PLIC, the source id. That is the design's reading (D11 and its open question on ARM and RISC-V), and the GIC and PLIC details here are from memory: only the ACPI form exists in this tree and nothing of the other two is built or checked. Once `Isa` was gone, `kind` chose between one thing and nothing, so it goes with it; a platform whose sources need more than one number would replace the call with the specifier form that D11 turned down, as one change to a young ABI.
- **Who turns a device's number into the platform's.** Whoever reads the firmware's tables, in user space: the route service (`ours.acpi.Route`, `IsaIrq`) for an ACPI platform, the same role reading a device tree elsewhere. A driver walks three numbers in order: the device's (ISA IRQ 1 for the keyboard), then the route service, then the platform's pin (GSI), then `interrupt_map`, then the kernel's own number for the line (a virq, deliberately never the GSI, D3), then `interrupt_create`. Each number has one owner, and the kernel is given only the last two.
- **What was given up: the second oracle.** `check_acpi_route()` used to put the service's answer for each ISA number against the kernel's: the service's GSI, given to `interrupt_map` as a `Gsi`, had to name the line `interrupt_map(Isa)` named for the same ISA number. Those are two translations of one MADT written apart (the kernel's `platform::isa_irq_line()` in C++, the service's `isa_irq_to_gsi()` in `system/service/acpi-route`), and a disagreement between them was a bug in one of the two. With `Isa` gone, no program can ask the kernel's translation, so **that independent comparison is given up, and nothing replaces it for ISA 1 to 15 but the checks listed below.** In particular a service that gets the override of, say, ISA 5 wrong passes `routing_checks` as long as the GSI it names is a pin with a line. What is kept: the service's GSI for each ISA number is a pin the kernel has a line for (`NoResource` would say it is not); QEMU's q35 is wired as its tables say, which the repository already encoded (ISA 0 on GSI 2, and ISA 1 and ISA 3 on GSI 1 and GSI 3, which the old checks and the pc-ps2 record state); the cascade, a number past the ISA lines and `0xFFFFFFFF` are `NotFound` from the service; the ECAM window. And one place where the kernel's translation still shows through: the line of the service's GSI for ISA 0 is the kernel's own timer line (`interrupt_create` answers `AlreadyBound`), because the timer code takes that line from `platform::isa_irq_line(ISA_IRQ_PIT)`. Each half is still checked against the MADT itself, the source of truth: the kernel's by `pinlines.test` (28 checks, in the kernel, unchanged) and the service's by `acpi-route-test` on crafted tables (host). Why that is acceptable: the kernel never consumes the service's answer, so a wrong GSI cannot reach a device or another driver's line (a line the kernel holds answers `AlreadyBound`, one that does not exist `NoResource`); it shows as a driver whose interrupt never arrives, as the keyboard check of `boot_check` shows for ISA 1; and before this change the two agreed for every ISA number on the machine the images boot (`docs/changes/261031/acpi-route.md`). What it costs: a bug that moves an ISA number other than 0 on a machine whose MADT does that is no longer caught by the tree's tests. Rejected to keep the oracle: a second form of the call for tests (it would keep the ISA numbering in the ABI under another name), and a kernel-side shell command that dumps the ISA table for a test to read (a test of the kernel's table against itself).

**Change**
The order of the checks the handler makes is the old one minus the kind: the source is not `0xFFFFFFFF` (`InvArgs`), the resource is the interrupt root (`BadHandle`, `BadType`, `AccessDenied`), the source has a line (`NoResource`), and the number is written out. As before, the source is checked ahead of the resource, because each check names the argument it is about.

```cpp
// before
auto interrupt_map(ours::Uandle const resource, InterruptSourceKind const kind, u32 const source,
                   user_copy::UserOutPtr<u32> const out) -> Status {
    if (kind != InterruptSourceKind::Isa && kind != InterruptSourceKind::Gsi) {
        return Status::InvArgs;
    }
    ...
    switch (kind) {
        case InterruptSourceKind::Isa:
            virq = platform::isa_irq_line(source);
            break;
        case InterruptSourceKind::Gsi:
            virq = irq::virq_of_hwirq(source);
            break;
    }

// after
auto interrupt_map(ours::Uandle const resource, u32 const source,
                   user_copy::UserOutPtr<u32> const out) -> Status {
    ...
    auto const virq = irq::virq_of_hwirq(source);
    if (virq == irq::INVALID_V_IRQ_NUM) {
        return Status::NoResource;
    }
    return out.copy_to_user(virq);
```

The value `0` of the old kind was unused so that an argument left zero was refused; with no argument there is nothing to leave zero. `platform::isa_irq_line()` is not removed: the PIT and HPET code and `irq_test.cpp`, `interrupt_line_test.cpp`, `pin_lines_test.cpp` call it, and nothing but the system call stopped needing it. The checks of the three places that called the old form:

- `check_interrupt_map()` asks for GSI 2 (the timer's line, `AlreadyBound`), GSI 3 (free), GSI 16 (a PCI pin, level-triggered) and GSI 0 (the pin the timer was moved off, a line of its own), requires four different numbers none equal to its pin, and keeps `0xFFFFFFFF` (`InvArgs`, also with no handle), a source with no pin (`NoResource`), `0x100` and `0x103` (`NoResource`: as 8 bits they would be GSI 0 and GSI 3, which has a line), no handle (`BadHandle`), and that a refused call writes nothing. The Ioport root stays `BadType`. The `AccessDenied` for an Irq resource that is not the root is not asked here: a program has no call that makes one yet, and `resource.test` shows it (72 checks, unchanged).
- `check_interrupt_lines()` of the shell (the dbg image) takes its two lines with GSI 2 and GSI 3, and the cascade case becomes a source no pin has (`NoResource`) and `0xFFFFFFFF` (`InvArgs`).

**Reference**
None, this project's own design: `.agents/tasks/interrupt-system/DESIGN.md` D11, the call section, and `.agents/tasks/driver-manager/DESIGN.md`, the route service. Zircon's `zx_interrupt_create()` takes the platform's global interrupt number in the same way, with no kind (from memory, not re-read for this entry). The specifier form that was turned down is Linux's `irq_create_fwspec_mapping()` in `kernel/irq/irqdomain.c`, which both its device-tree and its ACPI paths call.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF, not the project's QEMU 11.1.1; no KVM; gn `1000 (1a7c151)`).

- `boot_check.py`, both images and the host tests, from the sources committed: build ok; dbg PASS (44 passed, 4 skipped, 0 failed); rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required); host tests 154 passed, 0 failed. In the rel log: `pc-ps2: ISA 1 is on GSI 1, edge, active high`, `pc-ps2: ready`, `pc-ps2: key a`, `pc-ps2: key left-shift`, `pc-ps2: key A`, `routing_checks: passed`, no FAILED line. In the dbg log (the shell holds the interrupt root there): `shell: interrupt_map(GSI 2) = Ok`, `shell: interrupt_create(GSI 2, the timer's) = AlreadyBound`, `shell: interrupt_map(a source no pin has) = NoResource`, `shell: interrupt_map(0xFFFFFFFF) = InvArgs`, `shell: interrupt checks passed`.
- Kernel tests (`scripts/kshell_run.py`), all ok and the same number of checks as before: `pinlines.test` 28 (it checks `isa_irq_line()` against the MADT, so the kernel's translation is still covered), `resource.test` 72, `interruptline.test` 59, `irqdomain.test` 39, `interrupt.test` 32, `irq.test` 72.
- Six mutations, each applied alone, built and booted, then restored byte for byte (checked by hash) and the final run above made from the restored sources. `--only rel --no-tests` except (3):

| Mutation | What the log showed | Caught |
|---|---|---|
| (1) the handler lets `0xFFFFFFFF` through | `interrupt_map(0xFFFFFFFF) = NoResource (expected InvArgs)` and the same with no handle (`BadHandle`) | yes |
| (2) the interrupt-root check dropped | `interrupt_map(no handle, 3) = Ok (expected BadHandle)`, `interrupt_map wrote its output though it refused`, and the Ioport root answering `NoResource` and `Ok` where `BadType` was due [5/6] | yes |
| (3) the handler ignores the source (every source gets GSI 3's line) | rel [3/6]: `two pins have one number`, `interrupt_create(... GSI 3) = AlreadyBound (expected Ok)`, `interrupt_map(200) = Ok (expected NoResource)` and others; dbg: `shell: interrupt_create(GSI 2, the timer's) = Ok (expected AlreadyBound)`, `shell: interrupt checks FAILED` | yes, on both images |
| (4) `check_acpi_route()` expects the timer on GSI 3 | `Route.IsaIrq: ISA 0, 1 or 3 is not on the GSI QEMU's q35 puts it on` [5/6] | yes |
| (5) the source is cut to 8 bits before the lookup | `interrupt_map(0x100) = Ok (expected NoResource)` and the same for `0x103` | yes |
| (6) the service answers GSI 3 for ISA 0, with check (4)'s expectation set to 3 so that only the kernel's timer line can object | `interrupt_create(the line of the service's GSI for ISA 0, the timer's) = Ok (expected AlreadyBound)` | yes |

- (6) is the only mutation that shows the one comparison with the kernel's translation that is left, and it needed the fixed expectation switched off to be seen alone: with both in place, a wrong ISA 0 is caught twice.
- Not caught, and so reported: a mutation that makes the handler check a range instead of the root (`validate_ranged_resource()` in place of `validate_root_resource()`) passes every check on this machine, since a program holds only roots; it will be visible once a call that narrows a resource exists (P2), as the first record of this file said. And, as the Cause says, a service whose answer for an ISA number other than 0 is wrong but names a pin with a line is not caught on this machine for the numbers 4 to 15 that the fixed expectations leave out (the cascade, ISA 2, excepted).
- clang-tidy (`prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86 --quiet`) on the four `.cpp` files changed: nothing on a changed line; what it reports is older (`shell_tests.cpp` lines 5077, 7304 and 7311, and `[[nodiscard]]` hints in `x86_pagings.hpp`). A planted misnamed function was flagged by the same invocation, so the silence is not a tool that did not run. `scripts/format_cpp.py` and `scripts/format_gn.py`: clean. `lint_docs.py --changed` on the documents edited: clean.
- `grep -rn InterruptSourceKind kernel system` finds nothing, and no mention of `Isa` as a kind of `interrupt_map` is left outside the change records and the history of the plans.

Not measured: any MADT but QEMU's (a machine that overrides an ISA number other than 0 above all); ARM and RISC-V, which have no implementation of the call to check; the project's QEMU 11.1.1 and real hardware; a caller outside this tree (there is none).

**Deliberately not done**
- A firmware-neutral specifier (controller and cells) in place of the number: D11 chose one number for one platform and keeps the specifier for the first non-x86 platform.
- Removing `platform::isa_irq_line()`, `ISA_IRQ_*` or `IsaIrqNum`: the kernel's timer code and the tests still use them; they are the kernel's own, no longer an ABI.
- A replacement for the second oracle (see Cause). The user is asked to confirm that giving it up is acceptable, or to ask for one of the two rejected forms.
- `AccessDenied` from a program: not reachable without a narrowing call (P2).
- The mode that pc-ps2 gives (`ModeDefault`, the pin as the kernel programmed it) is unchanged and still the user's to confirm (`docs/changes/261031/pc-ps2.md`).

**How it was found**
None, a planned step (interrupt-system D11, step 8b-3).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. ISA 0 and GSI 2 are one line on q35. Why does a system call that accepts both give two names to one line, and which of the two does the kernel keep as the call's argument, and why?
2. `check_acpi_route()` no longer compares the service with the kernel for ISA 1 to 15. What exactly did the old comparison catch that the fixed expectations and the `NoResource` check do not, and which one comparison with the kernel's translation survives?
3. With `kind` gone, the call has one number. What does that number mean on an ACPI platform and on a platform with a device tree, and what would a platform need before the single number stops being enough?
