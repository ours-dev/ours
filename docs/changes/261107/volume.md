# volume changes

## The logical volume layer: PV headers, the extent mapping, the LV as ours.block.Block (2026-11-07)

**Files**

- `system/lib/volume` — the PV header's codec and the extent mapping
  (`volume.hpp`/`volume.cpp`), and the LV block server
  (`lv_server.hpp`): the same `ours.block.Block` the device drivers
  speak, whose requests the mapping splits into runs and drives through
  one block client per PV.
- `system/service/volume-manager` — the component: scans the block
  devices root.cm routes to it for PV headers, assembles the group,
  serves the group's first LV, resident when the group never completes.
- `scripts/make_vg_disk.py` — the VG disk maker: the PV header at a
  device's first sectors, one image a linear VG, two images a
  two-legged VG.
- `system/service/btreefs/btreefs.cpp` — a device that names no
  partition table is a whole volume: the adapter offsets from 0 and the
  geometry converts 512-byte sectors to 4 KiB blocks at the boundary.
- `system/service/init/test/routing_checks/routing_checks.cpp` — names
  the third device shape: a logical volume's block 0 is the filesystem's
  superblock, and the raw block checks stop there (a raw write past it
  would be corruption, not a check).
- `system/service/init/pools.hpp` — the fourteenth component, and the
  handle budget's arithmetic answered: MAX_BOOT_NODES 200.
- `kernel/products/image/dev/BUILD.gn` — the routing: the drivers'
  block protocol to the volume-manager as ours.block.Pv/Pv2 (the use's
  `type` naming the real one), its LV as ours.block.Block to btreefs and
  routing_checks.

**Symptom**

Latent: the filesystem's addressing ended at a disk. A volume was a
GPT partition on one specific device, found by walking that device's
table -- no spanning, no striping, no layer where "the disk" is a
policy rather than a fact.

**Cause**

The block protocol's client had no consumer that re-exposed it. The
stack was device → filesystem, with the "volume" being a partition
range and the offset arithmetic living in the filesystem's own adapter.
What LVM is -- a mapping from logical sectors to (device, physical
sector), itself a block device -- had no layer to live in.

**Change**

`system/lib/volume`: a PV is extents (1 MiB); an LV is segments naming
(pv, first extent, count); a linear LV concatenates its segments, a
striped LV round-robins chunks over its legs. The header at each PV's
first sectors carries its own segments, double written, CRC'd over the
whole block with its own field zero. The group assembles from the PVs'
headers and refuses a second header of one PV; it is complete when
every segment's PV joined and every striped LV's legs are all present.
`map_runs` splits a request into the contiguous-on-one-PV pieces, and
the LV server issues one PV call per run -- a filesystem on top cannot
tell an LV from a disk and does not have to.

The component's scan is a resident service's: it retries until the
group completes or five seconds pass, a quarter-second between rounds
-- the root starts its children one after another, and a driver's block
protocol appears in its outgoing only once its first rescan has claimed
the disk, so the first scan always raced it; and a resident scanner
that reads a device thousands of times a second is a denial of service
to whoever shares it.

**Reference**

Linux: `device-mapper`'s linear and striped targets
(`dm-linear.c`, `dm-stripe.c`) -- the same segment table and chunk
round-robin this mapping hand-rolls; LVM2's metadata -- the PV header
with the VG's UUID and the per-PV segment list, double written, CRC'd.
The difference: LVM2's metadata is text with a command-supported
update path; ours is binary and written once by the maker, the round's
honest scope.

**Verification**

- Host suites: volume 15 (codec round trips, the encode's and decode's
  refusals one broken field at a time, the linear and striped maps
  against hand-computed answers, map_runs' splits, the assembler's
  limits), lv_server 5 (the seam-crossing read as two runs, the striped
  round-robin, the write landing on the mapped PVs' sectors, a failing
  PV failing the request).
- Coverage (cov_host, lines): volume 98.4% -- lv_server.hpp 95.6,
  volume.cpp 98.8, volume.hpp 100.
- boot_check: dbg 46 passed / rel 44 passed, 7/7 required, on four
  machine shapes -- no disk, one VG disk (linear LV mounted, the file
  checks on /disk pass), two VG disks (the two-legged group assembled,
  its first LV spanning both disks), a plain GPT disk (skipped by the
  scan as a non-PV, mounted by btreefs the old way).

**Deliberately not done**

- Multi-LV exposure: the component serves the group's first LV; the
  device tree (dev/class/block) is where the rest belong, a later
  round's.
- The striped LV on target is assembled and leg-complete but not
  served: the served LV is the group's first, and the stripe's mapping
  is host-proven (StripeMap, hand-computed round-robin). Serving the
  striped LV on target is a one-line follow-up once the demo asks for
  it.
- Metadata update and resize: the header is written once by the maker;
  an online re-striping is LVM2's other half.
- The PLAN's PV-as-GPT-partition point became PV-as-whole-device: the
  scan reads the device's first sectors, the GPT had no reader, and the
  three structures collided at the disk's head. Recorded as a deviation
  in the RESULT, with the GUID kept in the maker for the day a
  partition-backed PV returns.

**How it was found**

The scan-vs-claim race via the first two-disk boot's "no complete
group" against a virtio driver that claimed a second later; the
destructor-closed handle via routing_checks' BadHandle on a read whose
GetInfo had just passed; the units bug via btreefs's page fault at
cr2=0x4 (4096 4-KiB blocks asked of a 2-MiB LV); the maker CRC form
twice, once per language.

**The user's prediction**

None.

**Three questions you should be able to answer**

- What is an LV's segment, and how does a striped LV's chunk
  round-robin map a logical sector to a leg?
- Why does the volume-manager's scan retry on a timer, and what does
  the throttle protect the devices from?
- Why does routing_checks stop its raw block checks at a
  BTREEFS-signed block 0, and what would a raw write past it be?
