# acpi-route: the route service as a program, answering from the static tables

**Date**: 2026-10-31
**Status**: Done

## Request

The user approved on 2026-10-31 a first slice of the ACPI route service that needs no AML interpreter: it answers which GSI an ISA interrupt arrives on and where a PCI segment's configuration space is, from the tables the kernel hands user space as the read-only VMO `ours.boot.Acpi`. The logic of the two answers is done (`system/service/acpi-route`, `docs/changes/261031/acpi-route.md`). This plan is the program that serves them. Scope ends at a program that `routing_checks` can ask. Moving pc-ps2 onto it (done, `docs/changes/261031/pc-ps2.md`), and then removing `Isa` and `kind` from `interrupt_map` (done, `docs/changes/261031/syscall.md`), are the steps after. The AML interpreter and the questions only AML answers (`_PRT`, `_CRS`) are not here; the interpreter needs the user's approval. Design context: `.agents/tasks/driver-manager/DESIGN.md`, the route service.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. The protocol | one protocol, `ours.acpi.Route`, with `IsaIrq` and `EcamBase`; or one protocol for each | one: it is the name and the pair of methods the Driver Manager draft already uses | None | One, 2026-10-31, as recommended |
| D2. Who is offered it in this step | routing_checks alone; routing_checks and pc-ps2 | routing_checks alone: pc-ps2 moves with the removal of `Isa`, which is a step of its own | None | routing_checks alone, 2026-10-31, as recommended |
| D3. When there are no tables | the program exits and a route to it answers NotFound; or it stays and answers every question NotFound | exits: a route service with nothing to read has nothing to be, and the framework already answers NotFound for a component that has gone | None | Exits, 2026-10-31, as recommended |

## Steps

Each step is one commit, verified with `ours-verify`.

1. **The codec, `system/lib/ours.acpi`.** Hand-written, as `ours.resource` and `ours.examples.echo` are: the protocol `ours.acpi.Route` (`IsaIrq`: a ISA number in, a GSI, a trigger and a polarity out; `EcamBase`: a segment in, an address, a size and the first and last bus out), each request and reply with its encoder and decoder, the selectors and their ordinals, and an `ordinals_test.py` entry. Why first: both the program and the client need it, and it is pure and runs on the host. Verified by: a host test of every encoder and decoder (round trips, a txid out of range, a short buffer, a wrong ordinal, a reply with the wrong status form) and a mutation for each refusal. Commit: `feat(ours.acpi): the codec of ours.acpi.Route`. Done.
2. **The program and the check.** `system/service/acpi-route/src/acpi_route.cpp`: takes `VmaRoot`, gets the VMO from `ours.boot.Acpi`, maps it read-only, reads it with `TableDirectory::from_bytes()`, and serves `ours.acpi.Route` on a Loop through `ioservice::Outgoing`, as `examples/echo/echo_server.cpp` does (static storage, no heap). A `user_executable`, a manifest that uses `ours.boot.Acpi` and exposes `ours.acpi.Route`, an eager child in `root.cm` with its two offers (the VMO to it, the protocol to routing_checks), and `check_acpi_route()` in routing_checks. Why now: it is what step 1 is for, and the routing_checks check is the only place the two independent translations (this one and the kernel's) can be made to agree. Verified by: on the rel image, for each ISA number 0 to 15 but the cascade, the GSI the service answers, given to `interrupt_map(Gsi)`, gives the same line as `interrupt_map(Isa)` of the ISA number; the service's ECAM address for segment 0 equals the constant routing_checks already uses for QEMU's window; the cascade and a number past 15 answer NotFound, and so does a segment the MCFG does not list; and four mutations (the service answering the wrong GSI, serving on the wrong protocol, mapping nothing, and not offered to routing_checks), each failing routing_checks. Commit: `feat(acpi-route): the route service answers IsaIrq and EcamBase`. Done; the client (`isa_irq()`, `ecam_base()`) is in `ours.acpi` with it.

## Files

- `system/lib/ours.acpi/` — new, the codec: `include/ours_acpi/ours_acpi.hpp`, `src/ours_acpi.cpp`, `test/`.
- `system/lib/wire/test/ordinals_test.py` — the two selectors.
- `system/service/acpi-route/` — the program (`src/acpi_route.cpp`), its `BUILD.gn` (a user-only part beside the host-visible one) and its component manifest.
- `kernel/products/image/dev/BUILD.gn` — the child and the offers in `root.cm`.
- `system/service/init/test/routing_checks/` — the manifest use and `check_acpi_route()`.
- `docs/changes/261031/` — one record per step.

## Test plan

- Step 1: host tests and `ordinals_test.py`; `boot_check.py` unchanged.
- Step 2 (the cross-check against `interrupt_map(Isa)` was replaced by fixed expectations when `Isa` was removed: `docs/changes/261031/syscall.md`): `boot_check.py` on both images, where the new check runs in rel only (the dbg image shows no user-space output; `docs/changes/261031/init.md`), and the four mutations booted with `--only rel`.
- Not verifiable here: any firmware but QEMU's q35 under edk2, a machine whose MADT overrides differ, other architectures.
