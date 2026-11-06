# volume-manager: LVM — logical volumes under the filesystem

**Date**: 2026-11-06
**Status**: Proposed

## Request

实现 LVM，让文件系统不再直接和底层块设备通信，而是使用逻辑存储空间的逻辑编址。先评估可行性，再设计方案，最后实施，测试覆盖率达到 95% 以上。评估已过（feasibility: high — `prot::block` is already a pure logical-block interface and devices appear dynamically in the tree）；三个设计决定已拍板（2026-11-06）：映射档位 B（线性拼接 + 条带，快照不做）、元数据 A（盘上、双副本）、PV 边界 B（PV = 一个 GPT 分区，新类型 GUID）。

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Mapping tier | linear only / +stripes / +snapshots | linear + stripes, snapshots later | None | B (linear + stripes) |
| Metadata home | on-disk, two copies / startup manifest | on-disk | None | A (on-disk) |
| PV boundary | whole disk / a GPT partition | GPT partition, new GUID | None | B (partition) |

## Steps

1. **system/lib/volume — the on-disk metadata and the extent-mapping
   core** (this commit). Why now: everything above stands on the mapping;
   the 95% gate wants it host-testable first. The model follows LVM's:
   a PV is divided into extents (1 MiB); an LV is segments naming
   (pv index, first physical extent, count); a linear LV concatenates its
   segments, a striped LV round-robins chunks over its segment set. The
   metadata is a binary header at the partition's first LBA, double
   written (primary at LBA 0, secondary at LBA 1), CRC'd like the GPT's.
   Verified: the host suite — metadata round trips and refusals, the
   linear and striped mapping arithmetic against hand-computed answers.
2. **The LV's block service core + the volume-manager component**.
   Why now: the mapping exists, the protocol it serves exists
   (`ours.block.Block`, the same one the drivers speak). The core (in
   lib/volume) is a BlockServer subclass whose start() splits a request
   over the segments and drives one `prot::block::Client` per PV; the
   component scans routed devices for PV headers, assembles the VG, and
   serves each LV. Verified: the host suite over the fake kernel with a
   fake PV provider in the pump; boot_check green.
3. **make_vg_disk.py + on-target, two PVs, striped**. Why now: the
   service needs a disk to serve and the demo needs the stripe across
   two real devices (DISK + NVME_DISK). The tool writes the GPT with two
   "ours PV" partitions, the PV headers, and a striped LV; root.cm routes
   the devices to the volume-manager and its LVs to btreefs. Verified:
   routing_checks' block path passes over the LV; the stripe read/write
   lands on both disks (QMP or dbg kshell evidence).
4. **Records, full verification, merge/push**, as every round closes.

## Files

- `system/lib/volume/**` — new: the metadata codec, the VG/LV model, the
  extent mapping, the LV block-server core; host tests.
- `system/service/volume-manager/**` — new: the component.
- `scripts/make_vg_disk.py` — new: the VG disk maker.
- `system/service/btreefs/btreefs.cpp` — unchanged by design: it opens
  `ours.block.Block` and offsets to the geometry GetInfo names; the LV
  answers as a partition-sized block device. If the GPT walk moves to
  the volume-manager, only its route changes.
- `kernel/products/image/dev/BUILD.gn` — the routing: devices to the
  volume-manager, the LV to btreefs.
- `docs/changes/`, `.agents/tasks/volume-manager/RESULT.md`.

## Test plan

- `system/lib/volume` host suite: metadata codec round trips, the dual
  copy, CRC refusals, version refusals; the linear map (concatenation
  across segments and PVs), the stripe map (chunk round-robin, hand
  computed), the bounds; the block-server core over the fake kernel,
  its requests split across two fake PV providers. ≥95% lines on every
  touched file (cov_host).
- boot_check: dbg + rel, no disk (the volume-manager scans, finds none,
  stays resident — the pci-hotplug shape) and with a VG disk.
- On target: the striped LV across DISK + NVME_DISK; a write through
  btreefs lands on both PVs (read the raw sectors back host-side).
