# block-peripherals: the NVMe round's five steps

**Date**: 2026-11-03
**Status**: Proposed

## Request

The user's multi-peripheral direction, scoped with the user to the NVMe round: a polling NVMe driver serving `ours.block.Nvme`, dual-disk verified with virtio-blk beside it. AHCI and virtio multi-disk are later rounds.

## Decisions

See DESIGN.md's table; NVMe-with-inline-poll decided there on 2026-11-03.

## Steps

1. **Task workspace.** This document and DESIGN.md. Commit: `docs(block-peripherals): the design and the plan`.
2. **The device.** `scripts/qemu_boot.py` gains `NVME_DISK=` attaching `-device nvme,serial=ours0` (drive `if=none,id=nvme0,snapshot=on`). Verified by the QEMU command line in the run's log carrying the device. Commit: `feat(qemu): NVME_DISK attaches an NVMe controller`.
3. **The driver skeleton.** `system/driver/nvme/` (user_executable + manifest using Mmio/Iommu, exposing `ours.block.Nvme`), ECAM class-match discovery, BAR0 through `MappedMmio`, register file constants, controller bring-up to `CC.EN=1`/`CSTS.RDY=1`, admin queues live, Identify Namespace read — logged ("nvme: controller ready, NSZE …"), no block-server wiring yet, the protocol not exposed in the manifest until step 4 (the component starts, finds no device without NVME_DISK, exits 0 as virtio does). Verified diskless AND with NVME_DISK by the boot log lines. Commit: `feat(nvme): the controller, discovered and brought up`.
4. **The block service.** `NvmeBlockServer` on `//system/lib/block-server` (geometry from NSZE, start() = SQE + doorbell + phase poll + complete), root.cm exposes `ours.block.Nvme` to routing_checks, `check_nvme()` runs GetInfo + LBA-5 write/read-back with its own skip line. Commit: `feat(nvme): the block service on the polled completion`.
5. **Records.** `docs/changes/261103/nvme.md`, the driver's README, `RESULT.md`. Full verification rerun. Commit: `docs(block-peripherals): the record`.

## Files

- Created: `.agents/tasks/block-peripherals/{DESIGN,PLAN,RESULT}.md`; `system/driver/nvme/**`; `docs/changes/261103/nvme.md`.
- Changed: `scripts/qemu_boot.py`; `kernel/products/image/dev/BUILD.gn` (child, resource offers, nvme protocol offer); `system/service/init/test/routing_checks/routing_checks.cpp` (`check_nvme`).

## Test plan

Every step: `boot_check.py --out out/nvme.x86` green. Steps 3-4 run twice: diskless (`no nvme device, skipped` in the log, `routing_checks: passed`) and with `NVME_DISK=<64M raw>` (`nvme: controller ready`, GetInfo's geometry matches the file size, LBA-5 pattern read back, `routing_checks: passed`). The dual-disk run adds `DISK=<gpt volume>`: the virtio chain (btreefs mount, file checks) and check_nvme both run in one boot. clang-format/tidy on changed C++; pre-commit green.
