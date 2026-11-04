# block-peripherals: the NVMe round delivered — a second block peripheral, polled, on ours.block

**Date**: 2026-11-03

## Delivered

- `feat(qemu): NVME_DISK attaches QEMU's NVMe controller` (4897906f) — the
  device side: `-device nvme,serial=ours0` on a snapshot-on drive beside the
  virtio disk, `NVME_DISK=` in `scripts/qemu_boot.py`.
- `feat(nvme): the controller, discovered and brought up` (7c7d7d52) — the
  driver's bring-up: ECAM class-match discovery (01h/08h), BAR0 through
  `MappedMmio`, the page-aligned DMA region sliced into admin queues /
  identify page / I/O queues / one data page, CC.EN bring-up to CSTS.RDY
  with per-second keep-alives, Identify Namespace for NSZE.
- `feat(nvme): the block service on the polled completion` (2520cf95) —
  Create I/O CQ/SQ, `NvmeBlockServer` (the `block_server::BlockServer`
  subclass, synchronous `start()`), `ours.block.Block` served; root.cm
  offers it as `ours.block.Nvme` to routing_checks, and `check_nvme()` runs
  GetInfo + an LBA-5 write-read-back with its own skip and not-run lines.
- `docs(block-peripherals): the record` — this file, the change record
  (`docs/changes/261103/nvme.md`), and the driver's README.

## Verification

- Full `boot_check.py` (dbg + rel + host): diskless PASS (dbg 46 passed,
  rel 44 passed, 7/7 required) and dual-disk PASS (`DISK=` + `NVME_DISK=` in
  one boot — the virtio chain through GPT/btreefs to the file checks and the
  NVMe chain through GetInfo and the write-read-back, both silent-passing,
  `routing_checks: passed`, no not-run lines).
- The bring-up log names its progress: `controller ready, version 0x10400,
  doorbell stride 0x4` → `controller ready, NSZE 0x20000` → `serving
  ours.block.Block over 0x20000 blocks` (0x20000 = 131072 blocks = 64 MiB,
  the attached file's size).
- clang-format and clang-tidy clean on the changed C++.
- Not measured: real-hardware NVMe (QEMU's model only); no benchmark — a
  feature round.

## Deviations from the design

- The manifest exposes `ours.block.Block` (the shared protocol name), not
  `ours.block.Nvme`: the Nvme name lives on root.cm's offer (`as`), which is
  what a client's `/svc` sees. PLAN step 3's wording said the protocol was
  exposed "in the manifest" — it is, under the protocol's own name.
- `check_nvme` verifies block_size and a non-zero block count rather than an
  exact block count: the count names the attached file's size, not a
  constant.
- The plan's "128 KiB DMA region with per-slot data pages" landed as 9 pages
  (36 KiB): MAX_INFLIGHT=1 needs one data page, and the admin queues' 2-page
  slices are what the bring-up verified first.

## Found along the way

Four bring-up bugs, each named by evidence and fixed in the round; the full
stories are in `docs/changes/261103/nvme.md` ("How it was found"): the
7.5-second CSTS.RDY mirage (boot_check's verdict window, not the device),
AQA shifted twice, the completion's phase at bit 16 not bit 0 (and the
status field beside it), and the CID never stamped into the SQE. Nothing
unfixed left; nothing filed in `docs/repairs/`.

## Not done

- AHCI driver, virtio multi-disk, one-driver-many-devices: later rounds
  (DESIGN.md's table).
- Interrupts (kernel MSI, P3), the route service's PciIrq method, the block
  protocol's flush/trim, btreefs on the NVMe volume (needs the second
  Filesystem instantiation refactor), the I/O scheduler seat on the slot
  table — all recorded as the design's later work.
