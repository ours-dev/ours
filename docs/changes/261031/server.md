# server changes

## Title: the protocol-server base, so a new server starts at its methods (2026-10-31)

**Files**
- `system/lib/server/include/server/server.hpp` — `ProtocolServer<Derived, MAX_CLIENTS>`, the client set, the message pump, and the reply and epitaph helpers, header-only.
- `system/driver/virtio-blk/virtio_blk.cpp` — BlockServer rewritten onto the base: the class keeps only the dispatch and the two Read paths.
- `system/driver/virtio-blk/BUILD.gn` — uses //system/lib/server.

**Symptom**
Every hand-written protocol server re-pasted the same ~100 lines: the client entries and their Waiter plumbing (connect, release, rearm_failed), the Storage buffers, the pump (channel_read, decode_header, dispatch), and the write/epitaph helpers. echo's EchoServer had them, init's ResourceServer has them, the block driver's BlockServer had them — a fourth server would have pasted them again.

**Cause**
The plumbing was written per server from the start: echo is the tree's teaching example and was copied, and each new server copied the copy. The shape was stable across all three — the same client lifecycle, the same buffers, the same pump — which is the signal that it belongs in a library.

**Change**
`ProtocolServer<Derived, MAX_CLIENTS>`, a CRTP base (build-time polymorphism, as everywhere here — no vtable for the dispatch): the base owns the Client entries, the Storage, connect/release/rearm, the pump, and the helpers; the derived server provides `MAX_CLIENTS`, `on_message(channel, header, message) -> loop::Action`, and whatever state its methods touch. The header-error path (peer not speaking the protocol: handles closed, connection ended) and the "no free entry" path (epitaph NoResource) live in the base, because they were identical in every server.

The first migration is the block driver's BlockServer: the class went from ~170 lines to ~95, all of them its own dispatch. echo keeps its copy for now — the developer's guide's second chapter quotes echo's lines by number, so echo's migration goes with a guide edit, not silently.

**Reference**
The shape each of the three servers copied; Zircon's equivalent is a FIDL binding plus `async::Loop`, where the plumbing is generated rather than a base class. ours generates no bindings (hand-written codecs, D20), so the plumbing is a base instead.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 152. With a virtio-blk-pci and a disk: `routing_checks: passed` — the block-0 read through the protocol works on the base as it did on the copy. clang-tidy quiet.

**Deliberately not done**
- The dispatch is one hook, not a generated per-method table: the codecs stay hand-written (D20), and a table wants the protocol-description machinery that does not exist yet.
- echo and init's ResourceServer still carry their copies; echo's migration must update the guide's quoted line numbers in the same commit.
- No host test for the base itself: its logic is exercised end to end by every server that uses it, and a unit test would need the fake kernel the ioservice tests keep, which is not yet a shared test target.

**How it was found**
The user, watching the third server be written: "每加一个新的驱动或者新的 Server，就会有很多重复的步骤".

**The user's prediction**
None. The user picked the CRTP shape (A) from two options; the composition alternative (a ClientSet class, pump per server) stays available if the base outgrows itself.

**Three questions you should be able to answer**
- What does the base own, what does the derived server own, and which of the two does the outgoing directory talk to?
- Why is the base a CRTP template rather than a virtual base, and which of the two would cost a vtable where?
- Why did echo migrate later even though it was the first server, and what has to change in the same commit when it does?
