# volume-manager: delivered — the filesystem addresses logical space

**Date**: 2026-11-07

## Delivered

- `feat(volume): the on-disk metadata and the extent mapping` —
  system/lib/volume: the PV header's codec (double written, CRC'd like
  the GPT) and the extent mapping: linear concatenation and striped
  chunk round-robin, `map_runs`' request splitting, the group assembler
  and its completeness rule.
- `feat(volume): the LV as a block server, and the volume-manager
  component` — LvBlockServer (ours.block.Block over the runs, one block
  client per PV), the resident scanning component, btreefs's
  whole-volume path, routing_checks' third device shape, and the
  instance pool's fourteenth seat with its handle budget answered.
- `feat(volume): the VG disk maker, and the striped group on target` —
  scripts/make_vg_disk.py, the scan's throttle, the kept-client fix,
  and the nvme deallocated-read repair item (the nvme round's, filed
  not fixed).

## Verification

- Host suites: volume 15, lv_server 5 — all green.
- Coverage (cov_host, lines, the round's gate ≥95%): system/lib/volume
  98.4% — lv_server.hpp 95.6, volume.cpp 98.8, volume.hpp 100.
- boot_check: dbg 46 passed / rel 44 passed, 7/7 required, on four
  machine shapes — no disk, one VG disk (linear LV mounted, /disk file
  checks pass), two VG disks (the two-legged group assembled, its first
  LV spanning both), a plain GPT disk (skipped as a non-PV, mounted the
  old way).
- The delivered promise, end to end: the filesystem's addressing now
  ends at the mapping. btreefs opens ours.block.Block, the volume
  manager answers, and the sectors land on whichever disk the metadata
  names.

## Deviations from the design

- The PLAN's PV was a GPT partition of a new type GUID; the delivered
  PV is a whole device. The scan reads the device's first sectors, the
  GPT had no reader, and the header, the MBR and the entry array
  collided at the disk's head. The GUID stays in the maker for the day
  a partition-backed PV returns.
- The PLAN's on-target stripe is assembled (the two-legged group, legs
  verified complete) but not served: the component serves the group's
  first LV, and the two-disk boot's served LV is the linear one
  spanning both disks. The stripe's mapping is host-proven.

## Found along the way

- The scan races the drivers' own boot: the root starts its children
  one after another, and a driver's block protocol appears in its
  outgoing only once its first rescan has claimed the disk. The scan
  retries on a quarter-second throttle for five seconds.
- A client's destructor closes the channel it wraps: the scan's probe
  client closed the end its caller meant to keep, and the root-forwarded
  connection carries no duplicate right to re-open it with. The probe
  client is now the kept client, constructed in place.
- btreefs's whole-volume path initialized its adapter after its first
  use — the page fault was at cr2=0x4, a null object's field.
- The maker's CRC used the prefix form twice, once in C++ (Step 1) and
  once in Python (Step 3): the decode's form is the whole block with
  its own field zero, and the prefix is a different number.
- The nvme driver reads a fresh image's deallocated blocks as BadState
  under concurrent reads — the nvme round's defect, filed as
  docs/repairs/261107/nvme-deallocated-reads.md with the baseline
  control that isolates it.

## Not done

- Multi-LV exposure through the device tree; serving the striped LV on
  target (assembled and leg-complete, the mapping host-proven).
- Metadata update and resize: the header is written once by the maker.
- The nvme deallocated-read fix (filed, the nvme round's).
