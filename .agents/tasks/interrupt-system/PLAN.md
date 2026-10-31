# interrupt-system: build the interrupt system in phases, with the kernel's virtual interrupt number from the start

**Date**: 2026-10-31
**Status**: Proposed

## Request

The user asked (2026-10-31) for an overall map of the interrupt system's final shape, with what the kernel must be able to do and what user space must be able to do; for the plan to be split into a compromise that gets the system running now and the design expected later; and for user programs to use the kernel's virtual interrupt number and never handle a GSI. Then, because the weekly budget is short, to have it all written down as a complete plan with UML diagrams for review. The design is `DESIGN.md` beside this file.

Scope ends at P1 in detail. P2 to P5 are listed as steps to be planned when their triggers appear. Nothing in P2 to P5 is started, and none of it is needed for the virtio-blk driver.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Name of an interrupt in the user ABI | GSI; virq | virq | None | virq, 2026-10-31 |
| D2. How a program learns its virq | handles from a bus service plus `interrupt_map`; kernel enumeration | handles plus `interrupt_map` | None | handles plus `interrupt_map`, 2026-10-31 |
| D3. virq assignment in P1 | identity; linear domain with virqs not equal to GSIs | linear domain | None | Linear domain, 2026-10-31: the rule (virq is not the GSI) and the structure (linear, changing at P3) are separate; see `DESIGN.md` |
| D4. What `interrupt_map` requires | the Irq root; a resource covering the hwirq; a new slot | the Irq root | None | The Irq root, 2026-10-31 |
| D5. `RemapIrq` | keep then remove; remove now; keep | keep until pc-ps2 migrates, then remove | None | Remove now, 2026-10-31, the user's choice: as step I4b, after `interrupt_map` exists |
| D6. Test device | `DISK=` off by default; always on; none | `DISK=` off by default | None | `DISK=` off by default, 2026-10-31 |
| D9. Where the P1 driver gets a PCI function's GSI | q35's wiring rule; evaluate `_PRT`; try each pin | the rule, checked against a real interrupt in I5 | None | The rule, 2026-10-31; checked against the firmware's `_PRT` instead, I5 being dropped (D10) |
| D10. What I5 is in the tree | a minimal driver in `routing_checks`; a config-change interrupt through the QEMU monitor; none, hand over | the minimal driver | None | None, hand over, 2026-10-31, the user's choice |
| D11. The shape of `interrupt_map` for other platforms | one number, no `kind`, once `Isa` goes; a firmware-neutral specifier (controller and cells) translated in the kernel | one number now, the specifier when the first non-x86 platform arrives | None | One number, no `kind`, 2026-10-31, the user's choice: the specifier is too complex for one platform. `Isa` and `kind` go with the route service of P2 (step 8) |

## Steps

Each step is one commit and is introduced to the user before it starts, one at a time (CLAUDE.md). The numbers continue `.agents/tasks/driver-capabilities/PLAN.md`: its step 8 is split here, and its step 9 (the hand-over) comes after step I5.

Phase P1: PCI INTx for the virtio-blk driver, with the final numbers.

1. **I1. `DISK=` in the boot scripts.** `scripts/qemu_boot.py` adds `-drive file=$DISK,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0` when `DISK` is set, and `scripts/kshell_run.py` passes it through. Off by default, so `boot_check.py` is unchanged. Why first: the later steps and the driver need a device to test against, and QEMU already has one. Verified by: both images boot with and without `DISK`; with it, QEMU's monitor `info pci` lists the device. Commit: `feat(scripts): DISK=, a virtio-blk disk for the boot scripts`.
2. **I2. Every IO-APIC pin is programmed and has a line.** `platform_init_apic` programs the pins the MADT names, masked, with vector `PlatformIrqMin + GSI` as for ISA lines, and `irq::init_lines` builds a line for each. A GSI past `PlatformIrqMax - PlatformIrqMin` is skipped with a log line. No ABI change. Verified by: a kshell test that the pins have lines, that `interrupt_create` takes one and a second is `AlreadyBound`, and that a pin past the vector range has none. Commit: `feat(platform): every IO-APIC pin is a line`.
3. **I3. A linear domain and virqs.** `irq::Domain` (the empty `domain.hpp`) maps hwirq to virq and back for the IO-APIC's pins; virqs are assigned by the kernel and deliberately not equal to GSIs (D3). The interrupt path stays indexed by hwirq (vector minus base), and the control path (`attach`, `mask`, `configure`) by virq. `interrupt_create`'s `line` becomes a virq; pc-ps2, `routing_checks` and the kshell tests move to the new numbers; `RemapIrq` stays until I4b, now yielding a virq. Verified by: the existing interrupt tests pass on virqs, a test that virq and hwirq are inverse and unequal, and a mutation of the mapping that fails them. Commit: `feat(irq): a linear domain, and a line is named by its virq`.
4. **I4. `interrupt_map`.** The call in `DESIGN.md`, gated by the Irq root, with kinds `Isa` and `Gsi`; its documentation in `syscalls.gni`; `ours.resource.Irq` described as the way to the capability. Verified by: `routing_checks` for each kind, an unknown kind, a source that does not exist, a resource that is not the root, and `interrupt_create` on the result. Commit: `feat(syscall): interrupt_map, a source's number for the kernel`.
5. **I4b. `RemapIrq` is retired.** pc-ps2 asks `interrupt_map(root, Isa, 1)` and then `interrupt_create(root, virq, 0)`; the interrupt tests of `kernel/commands/interrupt_line_test.cpp` and `system/app/shell/shell_tests.cpp` move to `interrupt_map` too. The option bit `0x1` is kept reserved and is not reused, since option values are ABI: a create that sets it answers InvArgs. `platform::remap_isa_irq` is deleted: `platform::isa_irq_line`, added with `interrupt_map`, is the `Isa` kind's translation, and nothing else needs the other. Why a step of its own: it changes pc-ps2 and the tests, which I4 should not. Verified by: the keyboard check of `boot_check` on rel (`pc-ps2: key a` and `pc-ps2: key A`), the interrupt tests passing with the same checks as before, and a create with the retired bit answering InvArgs. Commit: `refactor(irq): RemapIrq is retired, interrupt_map names the line`.
6. **I5. A device's interrupt arrives. Not done in the tree (D10, 2026-10-31).** The virtio-blk driver on the other side is the first test of a device interrupt; the interfaces are handed over in step 7. What follows is the step as it was planned. With `DISK=` set, a program makes an interrupt on the virtio-blk's INTx (through `interrupt_map` and `interrupt_create(ModeLevelLow)`), triggers a request, and receives the interrupt. The first user is zcode's driver; an in-tree check is optional. Verified by: that program's report, and `ours-verify` unchanged. Commit: whichever repository the program lives in.
7. **Hand-over. Done: `.agents/tasks/driver-capabilities/RESULT.md`.** The R4 part of step 9 of `driver-capabilities`: the interface list for zcode, with the virq contract (obtain it, do not compute it) and the clear-before-ack rule.

Phase P2: route service and isolation. Not started.

8. A user-space ACPI component that evaluates `_PRT` and answers `(bus, device, pin) -> (GSI, trigger, polarity)` (microkernel-interrupts D4). Once it exists, `interrupt_map` loses `Isa` and `kind` (D11): pc-ps2 gets the keyboard's GSI from it instead of asking for ISA 1, and the call becomes `interrupt_map(resource, source, out)`. The item is in `docs/repairs/261031/syscall.md`. Its first part, the ACPI tables reaching user space as a read-only VMO served as the built-in `ours.boot.Acpi`, is done (`docs/changes/261031/acpi.md`, `docs/changes/261031/init.md`); the logic of its first slice, which needs no AML (a reader of the tables' directory, the GSI of an ISA line and the window of a PCI segment), is in `system/service/acpi-route` (`docs/changes/261031/acpi-route.md`); the program, its protocol and the interpreter are not started, and the interpreter needs the user's approval.
9. The narrowing syscall for resources (`docs/repairs/261030/kernel-object.md`, item 2), so a driver holds a resource over one virq.
10. Exclusive ranges (`docs/repairs/261030/kernel-object.md`, item 6).

Phase P3: MSI and MSI-X. Not started.

11. A vector allocator (a bitmap of the platform vectors) replacing the static `PlatformIrqMin + GSI`.
12. An MSI domain and chip; the MSI System slot; calls to allocate and release a block of MSI interrupts as handles.
13. MSI-X in the user-space PCI driver, falling back MSI-X, MSI, INTx.

Phase P4: several CPUs. Not started.

14. Per-CPU vector tables; a call to set a source's target CPU; migration when a CPU goes offline (`.agents/tasks/cpu-hotplug/`).
15. A user-space balancing service (optional); x2APIC; several IO-APICs.

Phase P5: hardening. Not started.

16. Storm protection and throttling on a level line nobody clears.
17. Per-source counters, a kshell `irq` command, a shell tool.
18. Interrupt remapping, with a real IOMMU (`driver-capabilities` repairs item 3 lists the stub's limits).

## Files

Created or changed by P1:
- `scripts/qemu_boot.py`, `scripts/kshell_run.py`: `DISK=` (I1).
- `kernel/platform/pc/interrupt.cpp`, `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/desc_table.cpp`: pins and lines (I2); the domain (I3).
- `kernel/include/ours/irq/domain.hpp`, `kernel/kernel/irq/domain.cpp`: the linear domain (I3, new content for an empty scaffold).
- `kernel/object/interrupt_event_dispatcher.{hpp,cpp}`, `kernel/lib/syscall/src/interrupt.cpp`, `system/public/ours/syscalls.gni`, `system/public/ours/interrupt.hpp`: virq in `interrupt_create`, `interrupt_map` and `InterruptSourceKind` (I3, I4).
- `system/driver/pc-ps2/*`, `system/service/init/test/routing_checks/routing_checks.cpp`, `kernel/commands/irq*_test.cpp`, `kernel/commands/interrupt_line_test.cpp`, `system/app/shell/shell_tests.cpp`: the new numbers and the new tests (I3, I4), and the move off `RemapIrq` (I4b).
- `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/lib/syscall/src/interrupt.cpp`, `system/public/ours/interrupt.hpp`: the retired option bit (I4b).
- `docs/changes/<date>/`, `docs/repairs/<date>/`, `.agents/tasks/driver-capabilities/DESIGN.md` and `PLAN.md`: one record per step.

## Test plan

- Per step, `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes on both images, and the kernel tests of the step run with `python3 scripts/kshell_run.py --out out/bringup.x86 <name>.test`.
- I2: pins with lines, `AlreadyBound` on a second attach, no line past the vector range.
- I3: virq and hwirq inverse and unequal; the old interrupt tests pass unchanged on virqs; a broken mapping fails them (a mutation).
- I4: `routing_checks` lists every answer of `interrupt_map`.
- I4b: the keyboard still arrives (`pc-ps2: key a`, `pc-ps2: key A` on rel), the interrupt tests pass with the same number of checks, and the retired bit answers InvArgs.
- I5: a device interrupt received end to end, with `DISK=`.
- A mutation for every new check, as in the earlier steps of `driver-capabilities`, and an independent read of the call that takes a user-chosen number, before it is committed.
- Not verifiable here: real hardware, MSI before P3, other architectures.
