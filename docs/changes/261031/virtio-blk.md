# virtio-blk changes

## Title: the block device is driven end to end: ring, request, completion, sector read (2026-10-31)

**Files**
- `system/driver/virtio-blk/virtio_blk.cpp` — the driver, rewritten onto the legacy (transitional) interface after the modern one; the queue depth written back before the ring is registered; the request reads sector 0 and checks the bytes against what the image carries.
- `system/driver/virtio-blk/BUILD.gn` — uses ours.resource.{Mmio,Iommu,Ioport}.
- `kernel/products/image/dev/BUILD.gn` — Ioport and Iommu offered to the driver.
- `system/lib/ioservice/src/loop_server.cpp` — a forwarded Open whose write the remote's channel refuses now tells the delegate (from the same session's macro-fs work).

**Symptom**
The device could be enumerated and configured but a submitted request never completed: the driver's poll timed out on every boot, and the completion was invisible wherever the driver looked for it.

**Cause**
Three stacked bugs, two the driver's and one of method:
1. The modern common config's queue section does not answer on this QEMU build (queue_size reads 0xFFFF, num_queues 0) — worked around by the legacy interface, whose registers answer.
2. The descriptor-writing helper dropped the field offset and wrote the lengths over the addresses: the device was DMA-ing to and from physical 0x10 and 0x200 — the IVT and low memory — instead of the request buffer. Found by reading the descriptor table through the QEMU monitor: the flags and next fields were verbatim mine, the addresses were my length constants.
3. The verification scan checked the completion's length at the wrong offset, so even a correct completion looked absent.

**Change**
The legacy path: ECAM enumeration finds the device, its I/O BAR opens through `ioports_request`, and the registers — one field each, at one offset, one width — drive the whole flow: reset, ACKNOWLEDGE, DRIVER, features accepted as none, the queue selected, its maximum read (256) and the driver's depth (128) written back, the ring registered by page frame number, DRIVER_OK. The ring is one contiguous run of two pages from `vmo_create_contiguous`, pinned to one address with `bti_pin(Contiguous)`: descriptor table and available ring in page one, the used ring on the page boundary after (the legacy layout), the request header, sector and status behind it. One descriptor chain offered through the available ring, the queue rung, the used ring polled; the sector's first bytes are checked against what was written to the image. Every error path stops the device and observes it stopped before unpinning.

**Reference**
virtio legacy (transitional) PCI, registers at BAR 0: `virtio_ioport_write`'s QUEUE_PFN/QUEUE_NOTIFY cases (`hw/virtio/virtio-pci.c`); the split virtqueue of the legacy layout, used ring page-aligned after the available ring (`include/standard-headers/linux/virtio_ring.h`'s vring_size). QEMU's own trace events (`-trace enable=virtio_blk_handle_read`) served as the device-side ground truth, with HMP `xp` over guest physical memory as the memory-side one.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 151. With a virtio-blk-pci and a raw disk whose sector 0 begins with the bytes "OURS": `virtio-blk: used entry id 0x0 len 0x201` (512 data + 1 status), `virtio-blk: sector 0 starts "OURS-BLOCK-DEVIC"`, exit 0. clang-tidy quiet on the driver.

**Deliberately not done**
- The modern interface: its common config's queue section reads 0xFFFF/0 on this QEMU build (see `.agents/tasks/storage-layer/QEMU-CFG-FINDINGS.md`); the legacy interface needs none of it. The addendum's one-way-visibility question is thereby answered: there was no kernel mapping problem — the device was never writing into the region because the descriptors told it not to.
- Interrupts: polled, until the kernel names PCI lines (R4).
- The Block protocol, GPT and an on-disk filesystem: the next milestones.

**How it was found**
The QEMU monitor's `xp` over guest physical memory, reading the descriptor table while the driver was stuck: the flags and next fields were exactly the driver's, the addresses were the driver's length constants — the helper's missing field offset, visible in one dump. The same tool had earlier cleared the kernel of suspicion: `lookup_contiguous` walks the VMO's own pages, so the pin address and the CPU mapping are the same frames by construction.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the legacy ring need two contiguous pages when the modern one needed three, and what decides where the used ring starts in each?
- What made the device's completions invisible while its request processing was visible, and which tool showed it first?
- Why must the driver stop the device and observe it stopped before unpinning, and which kernel mechanism catches it if it does not?
