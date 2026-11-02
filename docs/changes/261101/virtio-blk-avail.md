# virtio-blk-avail changes

## Title: the available ring's index is a free-running counter, not a slot number (2026-11-01)

**Files**
- `system/driver/virtio-blk/virtio_blk.cpp` — the avail ring's idx is
  written as the request count truncated to 16 bits, not the count modulo
  the queue's size.

**Symptom**
The disk workloads of the copy-on-write filesystem killed QEMU with
`Virtqueue size exceeded` on the 256th block request of a boot, every time,
reproducibly. Before that workload existed no boot ever issued 256 block
requests, and the defect slept through every check that ever ran.

**Cause**
The available ring's idx field is a free-running u16 counter: it counts the
chains ever offered and wraps at 65536, the device keeping its own
free-running cursor and comparing the two with 16-bit arithmetic. The driver
wrote it as `(m_offered + 1) % LEGACY_QUEUE_NUM` -- the index of the ring's
entry slot, a different quantity that wraps at the queue's size. For the
first 255 requests the two agree; at request 256 the driver wrote 0 where
the device waited for 256, the device read the ring as having moved
backward, and its walk of the "new" chains -- entries long since freed and
rewritten -- ran past the queue's size, which QEMU answers fatally. The
entry slot's index (`avail + 4 + 2 * (m_offered % LEGACY_QUEUE_NUM)`) was
and is correct; only the counter was wrong.

**Change**

```text
// before: the counter reset at the queue's size
auto const offered = static_cast<u16>((m_offered + 1) % LEGACY_QUEUE_NUM);
*... (avail + 2) = offered;
// after: free-running, like the device's own cursor
*... (avail + 2) = static_cast<u16>(m_offered + 1);
```

**Reference**
Virtio 1.0 (and legacy) split ring: `avail->idx` is "a 16-bit idx wrapping
at 65536", the driver incrementing it per offered buffer -- the same
free-running rule as the used ring's idx, which this driver's wait loop
already followed (`m_seen` counting up and compared through u16). The
misreading copied the ring-entry index into the counter's role. Linux's
`virtqueue_add()` (`drivers/virtio/virtio_ring.c`, `vq->avail_idx_shadow++`)
is the free-running form.

**Verification**
The failing disked boot reached request 256 and died, every time; with the
fix the same boot runs to `routing_checks: passed`, and a second boot on
the same volume -- the request count well past 512 -- passes again. Host
156 tests, dbg PASS (44), rel PASS (42, 6/6 required), host-tests PASS 157
are unaffected (the diskless images never reach 256 requests).

**Deliberately not done**
- No stress test at the wrap in the host suite: the wrap lives in the
  driver's interaction with the device, which the host fake cannot see; the
  disked boot at 256+ requests is the regression check, recorded here.

**How it was found**
A one-line witness in `submit()` printing `m_offered`, `m_seen` and the
ring's idx per submit: the last line before QEMU's error read
`offered 256 seen 255 avail 0` -- the counter resetting exactly at the
queue's size, which no free-running counter does.

**The user's prediction**
None. The user said "continue" into the debugging session.

**Three questions you should be able to answer**
- Why did the first 255 requests of every boot work flawlessly with the
  wrong formula, and what would a boot with exactly 256 requests leave on
  disk?
- The entry slot's index is `m_offered % queue_size` while the counter is
  `m_offered + 1` -- why is one a modulo and the other not?
- How does the used ring's wait loop already embody the free-running rule
  the avail write was missing?
