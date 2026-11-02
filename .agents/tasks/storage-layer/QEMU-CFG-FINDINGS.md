# storage-layer: the virtio common config's queue section reads wrong -- evidence for the kernel/QEMU side

**Date**: 2026-10-31
**Status**: Open (needs the author of the driver capabilities, or a QEMU semantics decision)

## What works

The userspace virtio-blk driver (system/driver/virtio-blk, exp/fs-service) runs end to end up to
feature negotiation on QEMU's virtio-blk-pci:

- ECAM enumeration of bus 0 (four functions found, vendor 0x1af4 / device 0x1001 or 0x1042 with
  `disable-legacy=on`), BAR sizing including a 64-bit BAR, the virtio capability list read
  correctly (common/isr/device/notify, all in BAR 4);
- BAR 4 mapped through vmo_create_physical + vma_map (above 4 GiB: 0x800000000);
- feature negotiation: device features read 0x30007e54 / 0x101 (VERSION_1 present), status writes
  and read-backs work (3 after ACK|DRIVER, 11 after FEATURES_OK), on the modern-only device;
- the device config's capacity read through its capability: 0x20000 for a 64 MiB disk -- the one
  independently known value, and it is right.

## What does not

Two registers of the common config do not hold what the specification and QEMU's own state say:

- `queue_size` (u16 at common+0x1A) reads 0xFFFF (modern-only device, before and after reset);
- `num_queues` (u16 at common+0x14) reads 0x0000.

Both on the transitional (0x1001) and the modern-only (0x1042) device.

## The evidence

1. QEMU monitor ground truth (`info pci`, then `xp /40bx 0x800000000` while the driver was at the
   same point): bytes 0x10-0x13 = FF FF 00 00 (msix_config = NO_VECTOR), bytes 0x14-0x17 = 00 00
   0B 00 (device_status = 0x0B -- the driver's own status, correct), bytes 0x18-0x1B = 00 00 FF 00
   (queue_select 0, queue_size = **0x00FF = 255**), bytes 0x1C-0x1F = 00 00 0B 00.
2. The driver's own reads, same boot, same mapping: msix_config (u16@0x10) = 0xFFFF -- agrees with
   the monitor; device_status (u8@0x16) = 0x0B -- agrees; queue_size (u16@0x1A) = 0xFFFF -- the
   monitor says 0x00FF. num_queues (u16@0x14) = 0 -- the monitor agrees.
3. The values change with time in a way registers should not: queue_size read 0x00FF in the
   monitor after the driver reset and exited, 0xFFFF in the driver before and after its own reset,
   in two boots. 0xFF is not a legal ring size (the specification wants a power of two).
4. A u32 read at 0x10 returns 0x0000FFFF: the msix_config value in the low half. A u32 read at
   0x14 returns 0. A u32 read at 0x04 (device_feature, a real u32 field) returns 0x00000101 -- a
   whole-word field reads whole. It looks as if every access is served by the handler of the
   field that starts at its address, and bytes past a field's own width are the handler's padding,
   not the next field's bytes. Under that model queue_size's u16 at 0x1A is served by the
   queue_size handler and returns 0xFFFF for a queue the device considers unconfigured.
5. Narrow (u8/u16) and wide (u32) accesses were both tried, natural and composed; the values above
   are stable across all four shapes. Access width is not the variable.
6. The queue section (0x18-0x3F) reads as a mirror of the status block every 8 bytes in one dump
   (0x1C-0x1F == 0x14-0x17), which the specification's layout cannot produce.

## What the driver needs

One of:

- the correct reading of this QEMU build's common config queue section (which offset, which
  width, at which point of bring-up queue_size and num_queues become valid), or
- a confirmation that this QEMU build has a device-side quirk and the interim driver should take
  the queue size from somewhere else (a build constant, the legacy interface's queue_num register
  at BAR 0+0x0C -- which needs the legacy path, or 2 contiguous pinned pages for its ring, which
  needs vmo_create_contiguous).

## Reproduce

```text
python3 scripts/qemu_boot.py -monitor unix:/tmp/qemu-mon,server,nowait \
  -device virtio-blk-pci,disable-legacy=on,drive=hd0 \
  -drive file=/tmp/disk.img,format=raw,if=none,id=hd0
# in another shell, after the boot settles:
#   echo 'info pci'        | socat - unix:/tmp/qemu-mon
#   echo 'xp /40bx 0x800000000' | socat - unix:/tmp/qemu-mon
# the driver's own lines are in the boot log under "virtio-blk:".
```

The driver's bring-up stops at `virtio-blk: failed: OutOfRange` because queue_size reads 0xFFFF
and no ring of that size fits the one-page pin the driver is allowed before vmo_create_contiguous
lands.


## Addendum (2026-10-31): the legacy path works, and the writes are one-way

The driver was rewritten onto the legacy (transitional) interface after the modern common config's
queue section stayed unreadable, and the legacy registers all answer: queue_num reads 256, the PFN
register reads back exactly what was written, status transitions read back. The device then
**processes and completes the driver's request** — QEMU's own trace shows
`virtio_blk_handle_read ... sector 0 nsectors 1` followed by `virtio_blk_req_complete ... status 0`
for the driver's request (distinguishable from OVMF's earlier partition probes by position and req
object).

But the device's writes are invisible to the driver, while the driver's writes are visible to the
device — a one-way asymmetry, pinned down with the QEMU monitor reading guest physical memory while
the driver was stuck in its poll:

- The driver's available-ring write IS in QEMU's RAM: `xp` shows `idx = 1` at region+0x1002, the
  exact offset the driver wrote, every boot.
- The descriptor table at region+0 IS the driver's (flags and next fields verbatim) once a
  descriptor-writing bug of the driver's own was fixed (a helper dropped the field offset and
  wrote lengths over addresses).
- The device's sector data and the used-ring entry are in NEITHER the computed used ring
  (region+0x2000: all zero through the monitor), NOR anywhere in the whole 12 KiB region (a scan
  for the used entry and for the disk's on-sector magic found nothing), NOR at the alternative
  used-ring candidates probed outside the region.

So: guest → device reads and writes are delivered; device → guest writes into the pinned
contiguous region are not observable by the guest CPU, though the device demonstrably processed
the descriptors that name that region. The region is 3 pages from `vmo_create_contiguous`, pinned
with `bti_pin(Contiguous)` to one address, and mapped with `vma_map` of the same VMO; the PFN
register reads back the region's frame number. The question for the kernel side: **does the CPU
mapping of a pinned contiguous region (or of a contiguous VMO generally) alias the frames the
device sees — and if it does, why would QEMU's device-side writes at those physical addresses not
land in them?** A minimal reproduction is the driver as committed (exp/fs-service, HEAD) booted
with a virtio-blk-pci and a raw disk whose sector 0 starts with the bytes "OURS"; the boot log
ends `virtio-blk: failed: TimeOut`, and a monitor `xp` of region+0x2000 (the address the driver's
log prints as the used ring) shows zeros while the trace shows the completion.


## Addendum 2 (2026-11-01): the legacy path works, and every read's DMA is 16 bytes short

The modern path's question is moot -- the driver moved to the legacy interface, where every
register answers. The driver drives the device end to end: ring from `vmo_create_contiguous` pinned
`Contiguous`, write and read requests submitted and completed, the data verified against a magic
written to the image. One defect remains, surgical and deterministic:

**The device's read DMA delivers all but the LAST 16 BYTES of the data buffer.** A 512-byte read:
bytes 0-495 correct, 496-511 zero. A 1024-byte read (2 blocks): 0-1007 correct, 1008-1023 zero.
The last 16 bytes stay whatever the buffer held before (zeros, on a freshly created contiguous
region) -- the first differing byte always sits 16 bytes before the data's end.

Evidence, all from one boot:

- QEMU's own trace: `virtio_blk_handle_read ... sector 1000` then `virtio_blk_req_complete ...
  status 0` for the read in question -- the device processed and completed it.
- The disk image after the boot: sector 1000 holds the exact pattern the guest wrote earlier,
  **including bytes 496-511** (`od -j 512496`: `a5` x 16) -- the write's DMA read the guest buffer
  in full, and the file is correct.
- QEMU's RAM at the disputed bytes, read through the monitor (`xp`): **0x00 at the same offset the
  guest reports** -- the guest's CPU view and QEMU's RAM agree, so the guest's mapping is faithful
  and the zero is in what the device's DMA wrote (or failed to).
- The reply the client received carries the same bytes.

The write direction shows no such defect: the file received all 512 bytes including the tail.

The question for the kernel/QEMU side: **why does the read DMA stop 16 bytes short of the data
descriptor's end?** 16 bytes is the size of the virtio-blk request header (which sits in a
*separate* descriptor, desc[0], before the data in the same chain) -- or of whatever the QEMU
build's `virtio_blk_handle_read` subtracts. A minimal reproduction: the driver as committed
(exp/fs-service, HEAD), booted with a virtio-blk-pci and a raw disk whose sector 0 starts with
"OURS"; routing_checks fails with `first differing byte at 496` (1-block read) or `1008` (2-block
read), always exactly 16 before the data's end.
