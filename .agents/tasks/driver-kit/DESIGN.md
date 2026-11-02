# driver-kit: the runtime helpers every userspace driver restates by hand

**Date**: 2026-11-03
**Status**: Proposed

## Problem

The block-driver library took the protocol out of a driver's hands; the rest of a driver's preamble is still copy-paste. On the current baseline a new driver author duplicates, from `virtio_blk.cpp` and `pc_ps2.cpp`:

- the resource-acquisition ladder — `ours_resource::get` per protocol, each failure path hand-closing every handle taken before it; virtio-blk's unwind is quadratic (the 4th failure closes 3 handles) and its `fail` lambda re-rolls every later error path for 28 lines;
- the startup-handle guard pairs (`take_startup_handle` + "started without X" + return 1), duplicated in virtio-blk and acpi-route;
- the MMIO window mapping (`vmo_create_physical` → `vma_map` → close the window), virtio-blk's `map_mmio`;
- the DMA chain — `iommu_create` → `bti_create` → `vmo_create_contiguous` → `bti_pin` → `vma_map`, and the teardown order that keeps the kernel from quarantining what was pinned;
- the interrupt acquire idiom — `interrupt_map` → `interrupt_create` → close the root, the discipline both drivers state in comments and re-type;
- and virtio-blk still hardcodes `ECAM_BASE` (`0xE000'0000`) although the ACPI route service answers `EcamBase` today and pc-ps2 already proves the pattern.

A fourth driver author would copy all of it, with the teardown orders — the parts a mistake turns into a kernel quarantine — copied worst.

## Goals and non-goals

Goals:
- `//system/lib/driver-kit`: the ladder as an object (`Resources`), one-line startup handles, and the map/DMA/interrupt idioms as RAII objects whose destructors run the teardown orders once, correctly.
- The three drivers migrate onto it; the keyboard check on rel (`sendkey` → `pc-ps2: key a/A`) and the disked block stack prove nothing broke.
- virtio-blk takes its ECAM base from the ACPI route service, deleting its hardcoded constant.

Non-goals:
- The copier driver template and the guide's driver chapter — the next round, after this API settles.
- A helper for the Loop/Outgoing wiring tail: user space has no heap, the storages belong to the driver, and a helper would move lines without removing any.
- virtio-blk's GSI from the route service: the route answers `IsaIrq` and `EcamBase` only; a `PciIrq` method is the driver-manager design's, unbuilt.
- The driver-manager `Claim` shape (proposed): these helpers take handles their caller holds, which is the property that keeps them valid when `Claim` lands.

## Design

One header, `driver_kit/driver_kit.hpp`, namespace `driver_kit`, CRTP-free plain objects. The alignment rule throughout: helpers consume handles; only `Resources` acquires, and only through an injectable getter (default `ours_resource::get`), which makes the ladder host-testable.

- **`Resources`** — constructed with a getter (defaulting to the real one); `take(kind) -> Result<Uandle, Status>` records what it holds; `drop(handle)` closes one held root early, the discipline both drivers already document (a root is a connection from a finite pool; the driver's line and mappings outlive it); the destructor closes everything still recorded, newest first — the quadratic ladder becomes stack unwinding. `Kind` enumerates the five resource protocols a driver asks for.
- **`startup_handle(type, name)`** — `ort::take_startup_handle` plus the "started without X" line, `Invalid` mapped to an error status.
- **`MappedMmio`** — `map(mmio_root, paddr, size) -> Result<MappedMmio, Status>`: `vmo_create_physical` → `vma_map` (uncached is the kernel's rule for physical VMOs) → close the window handle, keeping the mapping; `.addr()`; destructor `vma_unmap`s.
- **`Bti`** — `create(iommu_root) -> Result<Bti, Status>`: `iommu_create(Stub)` → `bti_create`, both held; destructor releases. **`DmaRegion`** — `allocate(bti, bytes) -> Result<DmaRegion, Status>`: `vmo_create_contiguous` → `bti_pin(PermRead|PermWrite|Contiguous)` → `vma_map`; `.virt()`, `.phys()`; destructor `vma_unmap` → `pmt_unpin` → close, the order virtio-blk's `fail` lambda hand-maintains.
- **`Irq::take(irq_root, domain, source, options)`** — `interrupt_map` → `interrupt_create` → close the root on success; on failure the root is untouched (the caller's `Resources` destructor owns it). pc-ps2's AlreadyBound ritual is a kernel-behavior test, stays in the driver as it is.

The events, virtio-blk's preamble after the migration: `Resources resources;` → four `take`s (each `if (is_err()) return 1;`, the destructor closing what came before on any early return) → `startup_handle` ×2 → `MappedMmio::map` of the ECAM window → `Irq::take` → `Bti::create` → `DmaRegion::allocate` — the 28-line `fail` lambda deleted, because every failure path between here and `loop.run()` unwinds through destructors.

The driver-manager alignment: nothing here narrows or grants; `Resources` is the documented P1/P2 shape, and when `Claim(id)` lands it returns handles that feed `MappedMmio`/`Irq`/`DmaRegion` unchanged.

## Alternatives considered

- **A `ScopeGuard` primitive instead of named objects.** Rejected: the teardown orders differ per object (unmap→unpin→close; unmap; close), and a guard per handle recreates the ladder one level up. The named objects carry the order.
- **The wiring tail (port + Loop + Outgoing + serve + run) as a helper.** Rejected for now: the storages must live in the driver (no heap), so the helper would take five references and retype the same lines.
- **Folding `Resources` into a driver_main function** (the "startup skeleton" idea from the first survey). Rejected this round: it cannot own the storages either, and it would hide which capabilities the manifest must `use` — the part routing already gates.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Round scope | kit library only; kit + template; kit + template + guide chapter | kit library first | — | kit library first (2026-11-03) |

## Testing and verification

- Host: `Resources`' ladder on an injected getter — all acquired, a mid-ladder failure's destructor closes the prefix, `drop` releases early without double-close, the destructor closes the rest; plus the existing 160 host tests.
- Target: `boot_check.py` dbg and rel PASS, rel 7/7 required — the keyboard check (`sendkey` → `pc-ps2: key a/A`) exercises the migrated interrupt path live; disked, `routing_checks: passed` with no not-run line, on the migrated virtio-blk.
- Source: clang-format and clang-tidy clean; documents linted.

## Open questions

- None. The API shapes above are the plan's to falsify; the driver migrations are the test.
