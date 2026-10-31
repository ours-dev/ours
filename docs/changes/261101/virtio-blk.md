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
