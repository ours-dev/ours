# nvme

The userspace driver of QEMU's NVMe controller (the storage plan's second
block peripheral, after virtio-blk). It finds the controller by class code in
the ECAM window rather than by vendor and device — the class names the
family, and every vendor's controller is driven the same way — brings it up
on its 16 KiB BAR0 register file, and serves `ours.block.Block`, which
root.cm offers under the name `ours.block.Nvme`.

## The objects

- `Regs` — the register file through `MappedMmio`, every control a store:
  CAP, VS, CC, CSTS, AQA/ASQ/ACQ, and the doorbell array at 0x1000. CC is a
  32-bit register on this device, and the doorbell stride is CAP.DSTRD's
  `1 << (2 + DSTRD)` (4 on QEMU).
- `Controller` — the one page-aligned DMA region, sliced by constant offsets
  into the admin queues (2 entries each), the identify page, the I/O queue
  pair (4 entries each), and the one data page a request's blocks travel
  through. PRP1 names the data page, PRP2 stays zero: the largest transfer,
  7 blocks, fits one page.
- `Queues` — one submission/completion pair, polled. The admin pair and the
  I/O pair differ in slices, depths and doorbells alone. `submit()` stamps
  the command id into the SQE, polls for the matching completion (the phase
  bit at bit 16 of the completion's fourth dword; the expected phase starts
  true), rings the completion head doorbell.
- `NvmeBlockServer` — the block-driver library's subclass: `geometry()` is
  the Identify'd namespace size, `start()` runs one request to completion
  (SQE, doorbell, poll, `complete()`), the same synchronous shape virtio-blk
  has. One queue pair and one data page make the serialization physical.

## The events

Bring-up, in the order the specification fixes: CC.EN clear, then
AQA/ASQ/ACQ, the I/O queue entry sizes (IOSQES=6, IOCQES=4), then EN. The
ready waits are wall-clock with per-second keep-alives — the controller's
transitions run on its own timer (CAP.TO, 500 ms units, 7.5 s on QEMU), and
the console's silence detector must be fed. Identify Namespace (CNS=0,
NSID=1) reads NSZE; Create I/O CQ and SQ bring the I/O pair live; the
outgoing directory serves the block protocol until the machine ends.

A boot without the device finds no class-01h/08h function and exits 0 — an
answer, not a failure. On claim the driver also sets the device's command
register's memory-space and bus-master bits: boot firmware that probed the
device and gave up may have left them clear, and the controller then fetches
no commands at all.
