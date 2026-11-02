# driver-kit: the six steps that land the runtime helpers

**Date**: 2026-11-03
**Status**: Proposed

## Request

The user's driver-library direction, scoped with the user to the kit library first: `//system/lib/ddk` plus the three drivers migrated onto it. The copier driver template and the guide's driver chapter are the next round.

## Decisions

See DESIGN.md's table; the scope decision was made there on 2026-11-03.

## Steps

1. **Task workspace.** This document and DESIGN.md. Commit: `docs(driver-kit): the design and the plan`.
2. **The library's core.** Copier scaffold `system/lib/driver-kit`; `Resources` (injectable getter, `take`/`drop`, destructor unwind) and `startup_handle`; the root BUILD.gn gains the tests group. Host tests: the ladder's all-acquired, mid-failure, `drop`, and destructor paths on a fake getter. Commit: `feat(driver-kit): the resource ladder and startup handles`.
3. **The hardware idioms.** `MappedMmio`, `Irq::take`, `Bti`, `DmaRegion` — kernel-syscall paths with no fake to model them, proven by the migrations. Commit: `feat(driver-kit): the mapping, interrupt and DMA idioms`.
4. **virtio-blk migrates.** Resources, startup_handle, MappedMmio (ECAM window), Irq::take, Bti + DmaRegion; ECAM base from `ours_acpi::ecam_base(0)` (manifest gains `ours.acpi.Route` in `use`, BUILD.gn gains `//system/lib/ours.acpi:client`); the `fail` lambda and the hand-rolled ladder deleted. Verified by the full boot check and the disked run. Commit: `refactor(virtio-blk): the kit, and the route's ECAM base`.
5. **pc-ps2 and acpi-route migrate.** pc-ps2: Resources + Irq::take around its AlreadyBound ritual (kept verbatim, commented as the kernel-behavior test it is); acpi-route: Resources + startup_handle. Commit: `refactor(driver): pc-ps2 and acpi-route on the kit`.
6. **Records.** `docs/changes/261103/driver-kit.md`, the library README, `RESULT.md`. Full verification rerun. Commit: `docs(driver-kit): the record`.

## Files

- Created: `.agents/tasks/driver-kit/{DESIGN,PLAN,RESULT}.md`; `system/lib/ddk/**`; `docs/changes/261103/driver-kit.md`.
- Changed: `system/driver/virtio-blk/{virtio_blk.cpp,BUILD.gn}`; `system/driver/pc-ps2/pc_ps2.cpp`; `system/service/acpi-route/src/acpi_route.cpp`; `BUILD.gn` (tests group).

## Test plan

Every step: `boot_check.py --out out/dk.x86` green before its commit. Step 4 adds the disked run (`routing_checks: passed`, no not-run line); every run on rel exercises the migrated pc-ps2 through the keyboard check. clang-format/tidy on changed C++; pre-commit green.
