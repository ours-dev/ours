# syscall changes

## `interrupt_map` gives the number of the line for a source the firmware names (2026-10-31)

**Files**
- `system/public/ours/syscalls.gni` — the new entry `interrupt_map(resource, kind, source, out)`, with its documentation.
- `system/public/ours/interrupt.hpp` — `InterruptSourceKind`, `Isa = 1` and `Gsi = 2`.
- `kernel/lib/syscall/src/interrupt.cpp` — `syscall::interrupt_map()`.
- `kernel/object/resource_dispatcher.cpp`, `kernel/object/include/ours/object/resource_dispatcher.hpp` — `validate_root_resource()`, next to `validate_ranged_resource()`.
- `kernel/platform/include/ours/platform/isa.hpp`, `kernel/platform/pc/interrupt.cpp`, `kernel/platform/arm/isa.cpp`, `kernel/platform/riscv/isa.cpp` — `platform::isa_irq_line()`; `remap_isa_irq()` asks it.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/commands/resource_test.cpp`, `kernel/commands/pin_lines_test.cpp` — the checks.

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
- Three mutations, each applied alone, built clean and run, then restored and compared with the saved copy: the root test of `validate_root_resource()` turned off (`resource.test` fails 2 of 69); the Isa kind answered as if it were a GSI (rel fails, `5/6 required`: ISA 0 and GSI 2 differ, and the timer's line is taken); `interrupt_map` that never looks at the resource (rel fails: a missing handle answers `Ok`, and the output is written though the call refused). All caught.
- `scripts/format_cpp.py` and `scripts/format_gn.py` clean on every changed file. The ARM and RISC-V stubs were compiled as syntax only, with their own target triples (`-fsyntax-only`, `-Werror`, `-Wmissing-declarations`): clean.

Not measured: more than one IO-APIC; a machine where an ISA IRQ is moved to a pin with no line; ARM and RISC-V beyond the syntax check; real hardware; QEMU 11.1.1. The independent read of a call that takes a user-chosen number, which the plan asks for before such a call is committed, was not done before this commit: the user asked for it to be committed and pushed at once. It is to be done on the commit and any finding fixed in a following one.

**Deliberately not done**
- `RemapIrq` stays until step I4b (interrupt-system D5).
- Narrowing: a driver is handed an interrupt object and not a resource; narrowing is P2.
- MSI as a source kind (P3), and other architectures' kinds.
- The ACPI `_PRT`: the caller still has to know the device's GSI (open question in the design, to be decided before step I5).

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `interrupt_map` ask for the interrupt root and not for a resource that covers the source, and why can the kernel not tell which sources a narrowed resource covers?
2. ISA 0 and GSI 2 are one line on q35. Why does the call leave out ISA 2, and what would a driver that asked for it have been given?
3. A refused call must write nothing. Which of the three mutations shows the checks cover that, and what in the order of the checks makes the write the last thing that can fail?
