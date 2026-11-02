# block_server

The block-driver library: what a block device driver subclasses so that its
author writes the hardware and nothing else. The server parks each request
in a slot — the parked channel, generation, txid and extent — and answers it
when the driver's completion path calls `complete()`; the driver implements
its geometry and its hardware `start()`, and nothing of the protocol.

## What the driver writes

A class deriving from `block_server::BlockServer<Derived, MAX_CLIENTS,
MAX_INFLIGHT>`, with two functions:

- `geometry()` — what `Block.GetInfo` answers: the block size and the block
  count.
- `start(slot, data)` — run one parked request against the hardware. The
  slot's `lba`, `count` and `op` say what was asked; `data` is a Write's
  blocks, to be placed where the device reads them from (empty for a Read).
  Return `Ok` once the work is offered; an error is answered to the client
  inline and the slot is freed.

And its completion path, from any callback on the Loop thread — an IRQ
waiter once the kernel delivers interrupts into a Loop, or inline in
`start()` for a device the driver drives to completion: `complete(slot,
status, data)`. An Ok completion of a Read carries exactly `count ×
BLOCK_SIZE` bytes; `data` may view the driver's DMA mapping, `channel_write`
copies.

`MAX_INFLIGHT` is the tag depth — how many requests the hardware works on at
once. A request arriving at a full depth is answered `ShouldWait`, in its
own reply form, and the client retries. virtio-blk, the reference driver,
sizes it to 1: its DMA region holds one request's layout.

## What the library handles

The connection table, the `ours.block` decode and dispatch, the slot
accounting, the backpressure, and the completion replies — including the
check that a parked channel still names the connection it parked on: the
slot carries the client entry's generation beside the channel value, so a
client that left, and a handle value the kernel has since reissued, both
drop silently.

The slot table is also where an IO scheduler seats itself, should one come:
a queueing policy between slot allocation and `start()`, and a dispatch in
the completion path. Nothing here is paid for it in advance.

## Layout

```text
include/block_server/    public headers, reached as <block_server/...>
src/                     sources
test/                    host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/block-server`; its headers alone are
`//system/lib/block-server:headers`. The host suite runs under
`//system/lib/block-server:tests`, on the fake kernel of
`//system/lib/testing/fake_kernel`. The reference driver is
`//system/driver/virtio-blk`.
