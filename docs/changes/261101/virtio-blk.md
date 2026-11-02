# virtio-blk changes

## Title: the status descriptor sits one header short, so a read's data lost its byte at len-16 (2026-11-01)

**Files**
- `system/driver/virtio-blk/virtio_blk.cpp` — desc[2], the status byte's descriptor, now points right after the data (`buf_base + HEADER + count * SECTOR`), not one header before it.

**Symptom**
Every read delivered its data minus one byte at `data_len - 16`: a 512-byte read had byte 496 zero (and the driver reported Ok), a 1024-byte read had byte 1008 zero. The QEMU trace showed the device completing the request with status 0, and the monitor (`xp`) confirmed QEMU's own RAM held the same zero the guest saw — so the fault was in what the device DMA'd, not in the guest's mapping.

**Cause**
The three-descriptor chain was built as header (desc[0], 16 bytes at `buf_base`), data (desc[1], at `buf_base + HEADER`, `count * SECTOR` long), status (desc[2], at `buf_base + count * SECTOR`). The status address dropped the `+ HEADER` — it points 16 bytes into the data's tail instead of right after it. The device honours the descriptor, so it wrote the status byte over `data[data_len - 16]` and never touched the real status slot at `data_end`. The driver then read the status from the correct slot, which a freshly created contiguous region fills with zero, and `0 == VIRTIO_BLK_S_OK` made every request look successful. So the bug hid twice: the corrupt byte looked like a DMA that stops 16 short, and the wrong status looked like success. Writes masked it entirely: a stray status byte inside the request's own buffer is harmless, and the stale zero read back as Ok.

The tell was the three-sizes experiment (1, 2, 3 blocks, each pre-filled with its expected pattern): a DMA that stops short would leave the tail stale, but the 3-block read showed a single zero at 1520 with correct bytes after it — one byte overwritten, not sixteen lost. And 1520 = `data_end - 16` = `buf_base + count * SECTOR`, the address the descriptor actually carried.

**Change**

```text
// before: the status one header short, inside the data's tail
put64(2, 0, buf_base + count * SECTOR);
// after: right after the data, where the driver reads it back
put64(2, 0, buf_base + HEADER + count * SECTOR);
```

The rejected alternative was to keep the address and read the status from where the device writes it: that would bless a chain that violates the specification's own shape (status directly after data) and would break the day a second descriptor layout arrives.

**Reference**
Virtio legacy transaction layout, 2.6.11 "Device Requirements: Virtqueue Flush": for a read, the device writes the data and then a one-byte status directly behind it — the status descriptor's address is `data_addr + data_len`. QEMU's `virtio_blk_req_complete()` writes `status` to the last in_sg element's address, byte for byte what the descriptor says.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 152. With a virtio-blk disk attached: `routing_checks: passed` — block 0's OURS magic intact through Block.Read, the write check's pattern landed at LBA 1000 (confirmed in the disk file on the host: `virtio-blk write check` at offset 512000), and the two-block read back over 999–1000 matched, whose block-1000 tail includes the byte at overall offset 1008 this bug used to zero. clang-tidy quiet on the file.

**Deliberately not done**
- The poll loop's stale-zero luck is gone but the loop still polls rather than waits on an interrupt: that is R4's work, already planned on feat/driver-caps.
- `ours_block.hpp` still re-declares `ours.io/Node.Close` as `CLOSE`, which makes `system/lib/wire/test/ordinals_test.py`'s uniqueness check fail (pre-existing, run by hand). Whether the block codec should alias `ours_io::CLOSE` or stay standalone is a design call left to the user.

**How it was found**
Monitor dumps (`xp`) of the region after each failing read; then the discriminating experiment — three reads of 1, 2 and 3 blocks over a disk pre-filled with a different byte per block, the read buffer pre-filled with the same pattern. The tail dump (correct pattern bytes after the one zero) and the address arithmetic (`data_len - 16` equals `buf_base + count * SECTOR` for every size) pinned the status descriptor, which the then-uncommitted header-readback logging had already made suspect.

**The user's prediction**
None. The user delegated the locate-and-fix ("你自己定位自己修吧"), asking to be told if code had to change.

**Three questions you should be able to answer**
- Why did the driver report Ok on every corrupted read, and why did writes never show the bug at all?
- What single observation from the three-sizes experiment rules out "the DMA stops 16 bytes short", and what does it point to instead?
- The driver and the device disagreed about the status byte's address by 16 bytes; which one is the specification's arbiter, and why is answering that way the fix rather than a workaround?

## Title: the driver parks in interrupt_wait instead of spinning on the used ring (2026-11-01)

**Files**
- `system/driver/virtio-blk/virtio_blk.cpp` — `submit()` waits on an interrupt object instead of polling the used ring; `main()` maps GSI 23 and takes the line before DRIVER_OK; `LegacyRegs` gains `isr()`, whose read clears the device's level.
- `system/driver/virtio-blk/BUILD.gn` — the manifest uses `ours.resource.Irq` (the dev image's root already offered it to this component).

**Symptom**
Latent, cannot be triggered today. The poll was the interim shape by design ("Interrupts come when the kernel can name this device's line"), costing the whole CPU for the duration of every block request — a busy spin of up to a hundred million reads of the used ring's index per submit.

**Cause**
A driver that polls owns its device's progress but burns its CPU doing it; the interrupt is the same wait with the thread parked instead of spinning. The two mechanisms also differ in what unblocks the line: a level line (PCI INTx is level and active low) is masked by the kernel from each delivery until its ack, and taking one interrupt via `interrupt_wait` is the ack of the one before — so the driver must not only wait, it must also clear the device's own level, or the unmasked line re-fires before any new completion.

**Change**
`main()` maps the line through the root of the interrupt lines (`ours.resource.Irq`): `interrupt_map(root, Gsi, 23)` — on this chipset a root-bus function at slot S, pin P, wires to GSI 20 + ((S + P) & 3), and QEMU adds this disk's virtio-blk as the first PCI function, slot 3 pin A — then `interrupt_create(root, number, ModeLevelLow)`, then closes the root. `submit()` becomes:

```text
// before: spin until the used ring's index moves, 100M tries, TimeOut
for (tries < 100'000'000) { if (used.idx == m_seen + 1) ...; spin_hint(); }
// after: park; the entry, not the wait, ends the loop
for (waits < 100) {
    interrupt_wait(m_interrupt, nullptr);
    (void)m_regs.isr();          // clears the device's level
    if (used.idx == m_seen + 1) { m_seen += 1; return status_byte == 0 ? Ok : BadState; }
}
```

The loop guard stays because a wait can come back without this request's entry (one the last request left pending); the entry is the truth, the wait is only the nap. The line is taken before DRIVER_OK, and the root handle is dropped once taken — the same shape as the keyboard driver's.

**Reference**
`Zircon zx_interrupt_wait()` — a driver waits on an interrupt object the interrupt system handed it, and the wait is the ack. The line's name comes from the firmware, not the driver: ours plans a route service over ACPI's `_PRT` (the `acpi-route` first slice just merged), after which this GSI constant is the first thing to go. The kernel side is modelled on Linux's genirq (`kernel/irq`): level lines mask at delivery until ack.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 155. With a virtio-blk disk: `routing_checks: passed` — GetInfo, block 0's read, the write at LBA 1000 and the two-block read-back all served with the submitting thread parked in `interrupt_wait`, on the same boot where routing_checks itself holds an interrupt object on GSI 16. Latency vs the poll: not measured — performance work goes through the user first, item by item.

**Deliberately not done**
- The GSI is a constant (`GSI_DISK`, 23), not a lookup: the route service that reads `_PRT` is upstream's next slice, and until it lands a constant with its derivation in the comment beats a half-route.
- No interrupt latency benchmark: a baseline with the poll is already gone, and a fair one wants the R4 perf pass, which is planned separately.
- The wait stays inline in `submit()`, blocking the server's one thread: the shape the poll had. A device queue that serves requests back to back wants the interrupt bound to a port and the used ring drained per packet — that is the next shape, not this one.

**How it was found**
None. This was the planned R4 follow-up, done after the merge brought `interrupt_map`/`interrupt_create`/`interrupt_wait` in.

**The user's prediction**
None. The user was shown the mechanism (map, create, wait, the ack-on-wait rule, the level line) and said "改".

**Three questions you should be able to answer**
- Why does the loop keep its guard — what can make a wait come back without this request's used entry?
- What breaks if the driver skips the `isr()` read: what does the device hold, and when does the kernel's unmask meet it?
- Why is the line taken before DRIVER_OK and the root handle dropped right after — what does the design doc say a device driver should be handed instead of a number, and what does this interim shape cost?
