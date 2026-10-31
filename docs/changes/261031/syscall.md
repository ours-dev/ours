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
