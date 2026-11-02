# virtio-blk-protocol changes

## Title: the driver serves ours.block.Block, and a second component reads a block through it (2026-10-31)

**Files**
- `system/lib/ours.block/{include/ours_block/ours_block.hpp,src/ours_block.cpp,BUILD.gn,test/*}` — the protocol codec: GetInfo (geometry) and Read (blocks by number), Node.Close for fsio::close, and host tests for both methods' round trips and refusals.
- `system/driver/virtio-blk/virtio_blk.cpp` — the driver restructured from a run-once probe into a resident provider: the device state in one object, `read_blocks()` the synchronous request primitive, a BlockServer over ioservice::Outgoing serving the protocol on a Loop.
- `system/driver/virtio-blk/BUILD.gn` — exposes ours.block.Block; uses the outgoing and loop libraries.
- `system/service/init/test/routing_checks/{BUILD.gn,routing_checks.cpp}` — uses the protocol and reads block 0 when the image carries a disk; skips on a route that answers NotFound.
- `kernel/products/image/dev/BUILD.gn` — the protocol offered from the driver to routing_checks.
- `BUILD.gn` — the codec's host tests wired into the default build.

**Symptom**
The driver could read the disk but spoke to no one: a run-once probe whose answer went to the boot log. After this change, a second component asks for block 0 over a capability route and gets the bytes — the storage layer's first client-server split.

**Cause**
The wrap itself was straightforward; the two findings on the way were not:
1. The first device boot hung routing_checks: its check probed with a second connect whose "status" helper waits for an epitaph — which a working route never sends. The probe now reads the reply and treats NotFound/PeerClosed as "no disk, skipped".
2. `fsio::close` sends ours.io/Node.Close, and the BlockServer (a closed protocol) closed the connection without answering — the client's close read PeerClosed. Fixed by answering Node.Close: any server a client may fsio::close must answer it, whatever else the protocol speaks.

**Change**
The protocol: GetInfo → {block_size = 512, blocks = the device config's capacity}; Read(lba, count ≤ 7) → data. Read's reply data cap comes from the channel message size (20 bytes of status-and-header + 3584 of data). The server runs the device synchronously per Read — header, descriptor chain, doorbell, bounded poll on the same Loop thread — which one device queue makes true anyway; a second client waits behind the first. The descriptor helper that lost the field offset last change is gone with the rewrite (put32 now takes the field like its siblings).

**Reference**
QEMU's virtio-blk device config, capacity u64 at VIRTIO_PCI_CONFIG_OFF (0x14 without MSI-X enabled) behind the legacy registers; echo's EchoServer as the provider shape (component-framework design "The protocol subset").

**Verification**
`boot_check.py`: dbg PASS (44 passed), rel PASS (42 passed, 6/6 required — routing_checks skips the block check on the diskless images), host-tests PASS 152 (3 new codec tests). With a virtio-blk-pci and a disk: QEMU trace shows the driver's `handle_read sector 0 nsectors 1` for the routed client's Read, and `routing_checks: passed` with the magic check green. The driver exits 0 when no disk is attached, so the diskless images boot unchanged.

**Deliberately not done**
- Write: the protocol carries Read only, until a consumer needs more; the device path is symmetric and lands with GPT.
- Multi-queue, interrupts (polled), and async replies: one device queue serializes requests on the Loop thread, which is the correct shape until measurements say otherwise.

**How it was found**
The hang: routing_checks' process destroyed line never came while the shell was already up — the check was parked on a receive that a working route never answers. The close: `close = PeerClosed (expected Ok)` in the boot log, against a server that was demonstrably alive.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the BlockServer answer ours.io/Node.Close even though its protocol is ours.block, and what does the client see when it does not?
- Why can a Read run the device synchronously on the Loop thread without starving other clients, and when would that stop being true?
- Why does routing_checks skip rather than fail when the route answers NotFound, and what would silently break if it treated NotFound as success?
