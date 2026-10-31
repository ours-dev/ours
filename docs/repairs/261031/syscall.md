# syscall outstanding repairs

## 1. `interrupt_map` still names its source by a kind, and one kind is an x86 ISA number

**Severity**
Low: the call is correct today, but its ABI will have an outside user (the virtio-blk driver of `.agents/tasks/driver-capabilities/`) and each caller added makes the change dearer.

**Symptom**
Latent, cannot be triggered today. The syscall interface carries `InterruptSourceKind { Isa = 1, Gsi = 2 }` and a source number whose meaning depends on it. `Isa` is a legacy x86 PC numbering that the interface should not expose, and on a platform the kernel is built for, only one numbering exists, so the argument selects nothing once `Isa` is gone.

**Cause**
`interrupt_map(resource, kind, source, out)` was written with `Isa` because pc-ps2 knows only ISA 1 (the keyboard), and the MADT's overrides that turn it into a GSI are read by the kernel (`platform::isa_irq_line()`), not by anything in user space. The `Gsi` kind alone would do for every other caller. This item depends on `docs/repairs/261031/x86-apic.md` item 2 and on step 8 of `.agents/tasks/interrupt-system/PLAN.md`: until a user-space component resolves ISA 1 to a GSI, pc-ps2 has no other way to name its line. That component now exists (`system/service/acpi-route`, protocol `ours.acpi.Route`, checked against the kernel's translation in `routing_checks`); what is left is moving pc-ps2 onto it.

**Change**
Remove `Isa` and `kind` together (interrupt-system D11): `interrupt_map(resource, source, out)`, where `source` is the platform's own number for the line (a GSI on an ACPI platform). Concretely: delete `InterruptSourceKind` from `system/public/ours/interrupt.hpp`, the argument from `system/public/ours/syscalls.gni`, the switch and the kind check from `kernel/lib/syscall/src/interrupt.cpp`; pc-ps2 takes the keyboard's GSI from the route service; `check_interrupt_map` in `routing_checks` loses the unknown-kind and `Isa` cases; the hand-over note in `RESULT.md` is updated. `platform::isa_irq_line()` stays, because the timer code and the kernel tests use it. Ruled out: a firmware-neutral specifier with a controller and cells, which is in the DESIGN's Alternatives and is too complex for one platform; and writing GSI 1 into pc-ps2 now, which names the wrong line on a machine whose MADT overrides ISA 1.

**Reference**
None, this project's own design (D11). The specifier form that was turned down is Linux's `irq_create_fwspec_mapping()` in `kernel/irq/irqdomain.c`.

**Acceptance**
`grep -rn InterruptSourceKind kernel system` finds nothing; `routing_checks` asks `interrupt_map` for a GSI, a source with no line (`NoResource`), `0xFFFFFFFF` (`InvArgs`) and a resource that is not the root (`AccessDenied`); `boot_check.py` passes on both images, with `pc-ps2: key a` and `pc-ps2: key A` on rel.
