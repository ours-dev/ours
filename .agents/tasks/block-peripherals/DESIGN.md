# block-peripherals: the block service layer grows a second peripheral — NVMe

**Date**: 2026-11-03
**Status**: Proposed

## Problem

The block service layer serves exactly one peripheral family: virtio-blk's legacy interface, driven by one driver component that claims the first matching PCI function and serves `ours.block.Block` on QEMU's I/O-port BAR. Every other storage controller — NVMe, AHCI, anything modern — has no driver, and the block protocol's users (btreefs, fshost's mounts, routing_checks' disk suite) can only ever see one disk's blocks. "Multiple peripherals" needs a second real device family whose blocks the same `ours.block.Block` semantics deliver, testable in the same QEMU boot.

QEMU's `-device nvme` is that family: it lands on bus 0 of the q35 ECAM window the drivers already walk, its BAR0 is 16 KiB of MMIO the ddk's `MappedMmio` maps unchanged, and its completion model can run **polled** — the CQ's phase bit and head doorbell are ordinary MMIO stores — which sidesteps both kernel gaps on record (no MSI/MSI-X; no interrupt-to-Loop delivery). The block-server's slot table already names "an NVMe command id" as its completion mapping; this round proves it.

## Goals and non-goals

Goals:
- `system/driver/nvme`: a user-space NVMe driver as a component, serving `ours.block.Nvme` through the block-server library, completions polled inline in `start()` — the proven virtio shape.
- ECAM discovery by class code (01h/08h) instead of vendor/device, BAR0 mapped through `ddk::MappedMmio`.
- `qemu_boot.py` gains `NVME_DISK=` to attach the device; `routing_checks` gains `check_nvme()` (GetInfo + write/read-back on LBA 5), so the dual-disk boot verifies BOTH block endpoints in one run.
- The ddk exercised by a second, structurally different driver: no I/O ports, no interrupt line — `Resources` takes Mmio + Iommu only.

Non-goals:
- AHCI (largest driver; ich9's INTx pin A shares GSI 23 with the virtio disk) — next-round candidate.
- virtio multi-disk in one driver (N devices, N endpoints) — next round; this round's NVMe is the second family.
- MSI/MSI-X (kernel P3), interrupt-to-Loop delivery (filed), route-service `PciIrq` (driver-manager design) — polling needs none of them.
- btreefs on the NVMe volume (a second Filesystem instance) — the NVMe serves raw `ours.block.Block` this round.
- Namespaces beyond NS 1; multiple controllers.

## Design

The objects:
- `Resources` (ddk): Mmio + Iommu roots — Irq and Ioport are not needed by a polling driver with MMIO BARs.
- The ECAM walk (the ddk has no PCI helper yet; the walk is virtio's, matched by class instead of vendor): config offset 0x08 reads class base 0x01 / subclass 0x08; BAR0 is config 0x10/0x14, a 64-bit memory BAR (low nibble masked), 16 KiB.
- `MappedMmio` over BAR0: the NVMe register file — CAP at +0, VS at +8, INTMS/INTMC at +C/+10, CC at +14, CSTS at +1C, AQA at +24, ASQ at +28, ACQ at +30, SQ0TDBL/CQ0HDBL at CAP.DSTRD-strided offsets.
- One 128 KiB `DmaRegion`, page-sliced in a fixed layout: admin SQ (2 pages), admin CQ (2 pages), Identify page, IO SQ (2 pages), IO CQ (2 pages), then one page per slot for data — every slice page-aligned, which is what PRP needs (a 7-block transfer = 3.5 KiB ≤ one page, so PRP1 = the slice's page address and PRP2 = 0; no PRP list page).
- Bring-up in the legacy order NVMe fixes: CC.EN=0, wait CSTS.RDY=0; AQA (ASQS=1, ACQS=1); ASQ/ACQ base addresses; CC.IOSQES=6, IOCQES=4; CC.EN=1; wait CSTS.RDY=1 (bounded spin, CAP.TO names the ceiling in 500 ms units). Admin commands (Create I/O Completion Queue, Create I/O Submission Queue, Identify Namespace CNS=0/NSID=1) run one at a time on the admin queues, each with its own phase-bit poll.
- Per-request: a 64-byte SQE — opcode Read (02h) / Write (01h), NSID=1, CID = the slot index, MPTR=0, PRP1 = the slot's data page, PRP2 = 0 — written to IO SQ, SQ tail doorbell rung, then the CQ polled: an entry whose phase bit matches the expected phase is this command's (CID checked), its status word maps to `ours::Status` (success → Ok; else BadState unless a named NVMe status code maps closer), the CQ head doorbell rings, and `complete(slot, status, data)` answers the parked client — the virtio shape, one command outstanding, `MaxInflight = 1` until the pump grows.
- `NvmeBlockServer : block_server::BlockServer<NvmeBlockServer, 2, 1>`: `geometry()` answers NSZE × the LBA size Identify names (QEMU's default 512); `start(slot)` builds and runs the command.

The events: routing_checks opens `/svc/ours.block.Nvme` (root.cm offers it from `#nvme`, which root.cm's resource offers arm with Mmio + Iommu), sends GetInfo — `geometry()` from Identify; sends Read/Write — `start()` submits, polls, `complete()`s. A boot without `NVME_DISK=` finds no class-01h/08h function and answers NotFound at the server's own discovery — an answer, not a failure, as virtio-blk's no-disk path is.

The alignment with the driver-manager design: the helpers consume handles from `Resources`' ladder (P1/P2 shape); when `Claim(id)` lands, its Config/Bar/Interrupt handles feed the same `MappedMmio`/`irq::take` unchanged — and NVMe polling never needs the Interrupt one at all.

## Alternatives considered

- **Interrupt-driven completion via INTx.** Rejected for this round: `irq::take` would work (the device's pin maps to a GSI the kernel programs), but `start()` would park on `interrupt_wait` exactly as virtio does today — the interrupt object buys nothing over polling until the interrupt-to-Loop path lands, and skipping the Irq root proves the ddk's minimal shape.
- **MSI-X first.** Rejected: no MSI controller, no vector pool, no capability parsing — designed-for at kernel P3, weeks of kernel work before the driver starts.
- **AHCI alongside.** Rejected this round: the largest of the three drivers, and q35's ich9 INTx pin A shares GSI 23 with the virtio disk — a shared-line entanglement the interrupt layer's exclusivity rules would need settled first.
- **One driver, N devices (virtio×N shape) for NVMe too.** Rejected this round: one NVMe device in the dev machine keeps the round about the FAMILY, not about N-endpoint routing, which the virtio round will settle with real multiplicity.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Second peripheral | NVMe polling; virtio multi-disk only; add AHCI too | NVMe polling — the second real family, zero kernel change | — | NVMe polling (2026-11-03) |
| Completion | Inline poll in start(); INTx interrupt_wait | Inline poll — the interrupt object buys nothing over polling today | — | Inline poll (2026-11-03) |

## Testing and verification

- `boot_check.py` on every step: dbg/rel PASS, host 161+.
- The dual-disk run: `DISK=<virtio.raw> NVME_DISK=<nvme.raw>`, both attached in one QEMU; `routing_checks: passed` — the virtio chain (mount, files) AND `check_nvme` (GetInfo + write/read-back on LBA 5) both run; the verdict's not-run line stays quiet while the ask was served.
- A no-NVME_DISK boot: `routing_checks: passed` still, with `no nvme device, skipped` — the skip made observable the way the disk suite's is.
- clang-format/tidy clean; docs linted.

## Open questions

- None for this round; QEMU's NVMe device defaults (queue sizes, phase behavior) are read from the spec-mandated registers at run time, not assumed.
