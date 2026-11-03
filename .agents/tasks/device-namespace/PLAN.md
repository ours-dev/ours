# device-namespace: the round's five steps

**Date**: 2026-11-04
**Status**: Proposed

## Request

The user's round: one directory tree where every block device answers,
whatever driver drives it — the devfs shape (Fuchsia's driver_manager hosts
`/dev/class/block/000`), the registry publishing runtime entries, replacing
the per-driver `/svc/ours.block.Nvme` renames once it answers both disks.

## Decisions

See DESIGN.md's table (all four decided 2026-11-04).

## Steps

1. **The codec.** `prot::dev` — the Registry protocol (`ours.dev.Registry`):
   one two-way method, Publish(class, name, directory), the entry the
   registry mounts at `class/<class>/<name>`. The next member of the
   protocols library; verified by the host codec test.
2. **The registry.** `system/service/device-registry`: eager child serving
   `ours.dev.Registry` (protocol out) and the `dev` directory (directory
   out). A Publish mounts the published directory's server end at
   `class/<class>/<name>` in its ioservice tree; the tree answers opens by
   forwarding into the published channel, the leaf a service node — the
   /svc primitive. Unmount on the publisher's exit (the registry holds the
   server end; a dead driver's channel closes). Verified diskless: registry
   starts, publishes nothing, tree empty; routing_checks' dev check skips.
3. **The publishers.** virtio-blk and nvme publish one entry per device
   after bring-up (class "block", name "000"), through `prot::dev`'s client
   plumbing. Verified dual-disk: both entries in the tree, check_dev_tree
   runs GetInfo through each leaf and both answer.
4. **The checks.** `check_dev_tree()`: readdir `/dev/class/block`, expect
   one entry per attached disk (0 diskless, 2 dual-disk), open each leaf,
   GetInfo through it; the old per-driver checks (`check_block`,
   `check_nvme`) retire only once the tree answers both.
5. **The record.** docs/changes/261104/device-namespace.md, RESULT.md, the
   registry's README.

## Files

- Created: `system/lib/protocols/include/prot/dev.hpp` (+client/src/test),
  `system/service/device-registry/**`.
- Changed: `system/driver/{virtio-blk,nvme}/**`, root.cm offers,
  `routing_checks.cpp`.

## Test plan

Every step green on `boot_check.py --out out/ns.x86`; steps 3-4 also
dual-disk (`DISK=` + `NVME_DISK=`). The dual-disk boot is the round's
verdict image: both devices in one tree, both silent-passing.
