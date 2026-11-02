# block-server changes

## Title: the block-driver library: the server a driver subclasses, the hardware its only work (2026-11-03)

**Files**
- `system/lib/block-server/` — new library: `block_server::BlockServer<Derived, MAX_CLIENTS, MAX_INFLIGHT>`, the slot table, the `ours.block` dispatch, `complete()`, and the backpressure answer; scaffolded with copier from `tools/templates/cpp_library`.
- `system/lib/server/include/server/server.hpp` — `Client` gains `generation`, bumped on every take; the header comment documents the deferred-reply pattern.
- `system/lib/server/README.md` — the library's first README: what the base handles, the deferred-reply pattern's two rules.
- `system/lib/testing/fake_kernel/` — the fake kernel the ioservice tests kept, moved out to a shared testonly target (`git mv`, byte-identical modulo the header guard and a doc line).
- `system/driver/virtio-blk/` — the private `BlockServer` class deleted; the driver subclasses the library and is down to `geometry()` and `start()`.

**Symptom**
None — an enabling capability. The `ProtocolServer` base answers only synchronously: an `on_message` must reply before returning, so the one driver with slow work (virtio-blk) polls the device inline inside the request path, and an author bringing up an interrupt-driven device (NVMe's completion queues) had nowhere to put the completion at all. The only working example hid its server class inside `virtio_blk.cpp`, from which a second driver could copy nothing; the base had no test and no README.

**Cause**
The deferred-reply machinery was implicit in the Loop's single-threaded model — a reply is a `channel_write`, callable from any callback on the thread — but implicit is not usable: the parked request needs a place to keep (channel, txid, extent) while the hardware works, a buffer that no other callback overwrites, and a check that the parked channel still names the connection it parked on. None of it existed, and virtio-blk's inline poll was the workaround that proved the need.

**Change**

```cpp
// the driver's whole server, after the migration
class VirtioBlockServer final
    : public block_server::BlockServer<VirtioBlockServer, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    auto geometry() const -> block_server::Geometry {
        return {.block_size = ours_block::BLOCK_SIZE, .blocks = m_device->blocks()};
    }
    auto start(usize const index, std::span<u8 const> const data) -> Status {
        // run the parked request against the device; complete(index, ...)
        // before returning
    }
};
```

The slot model: `on_message` decodes, takes a free slot (channel, generation, txid, op, lba, count — the request copied out of the shared buffers, per the deferred-reply discipline), calls `start(slot, data)`, returns `Rearm`. The completion path calls `complete(slot, status, data)`, which encodes into the slot's own bytes — a READ's data may view the driver's DMA mapping, `channel_write` copies — and answers the parked channel. A full depth answers `ShouldWait` in the request's own reply form and keeps the connection; a parked channel that no longer names its client entry — the client left, or the kernel reissued the handle value — drops silently.

Two hardenings the test suite forced: complete() states the driver's side of the contract (an Ok Read carries exactly `count × BLOCK_SIZE` bytes; a violation is said on the console and ends the connection rather than encoding nonsense), and the backpressure answer carries the request's own reply form, not a Read's form for both kinds. The base's `Client.generation` is what the completion check reads: the kernel reissues handle values, and channel value alone does not name a connection.

The slot table is also where a future IO scheduler seats itself — a queueing policy between slot allocation and `start()`, a dispatch in the completion path (Linux blk-mq's shape; every scheduler there is a callback state machine) — nothing paid for it in advance.

**Reference**
The slot model is blk-mq's request/tag table at user-space scale: fixed slots, a depth the driver sizes to its device, completions matched by hardware id. The deferred-reply pattern is stated by ioservice's LoopServer for a parked Open; the base now states it for every server. The C++20-coroutine alternative was considered and rejected with the user across two rounds of questioning: the hardware-completion matching table is needed under either model, the no-heap userland would need a static coroutine-frame pool (the slot table wearing a handle), parked-frame cancellation is the dangling-frame classic, and the reference systems keep coroutines out of driver completion paths — it remains a candidate for a later client-side async API, decided separately.

**Verification**
Host: the fake kernel moved to `//system/lib/testing/fake_kernel` with the ioservice suites staying green; the new block-server suite, 9 cases on a `FakeDevice` behind a real outgoing directory on the Loop — inline GetInfo, the slot fields `start()` sees, a Read's data and a Write's status-only reply, the backpressure with both clients drained afterwards, the client gone while parked, the reconnected entry whose generation moved on, a refused `start()` answered inline, the two close paths — 160 host tests, 0 failed. Target: `boot_check.py` dbg PASS (45), rel PASS (43, 7/7 required); disked, the GPT suite runs and `routing_checks: passed` on the migrated driver. clang-format clean; clang-tidy run in step 7's final pass.

**Deliberately not done**
- The interrupt-driven completion: no path delivers an interrupt into a loop::Loop (object_wait_async unsupported on interrupts; interrupt_bind needs a BindToInterrupt port, which the Loop's is not) — filed as `docs/repairs/261103/kernel-interrupts.md`; virtio-blk completes in `start()`, as it always did, and the async path's proof is the host suite.
- The general driver kit (startup skeleton, PCI/IRQ/DMA helpers, templates, a guide chapter) — the next round, once this API settles.
- An IO scheduler: the slot table is its seat, nothing more.
- Client-side pipelining (btreefs stays synchronous) and C++20 coroutines, deferred with the user.

**How it was found**
None — a planned task, from the user's driver-library direction, scoped in conversation; the two hardenings were forced by the test suite (the backpressure form and the completion contract).

**The user's prediction**
None. The user asked twice whether C++20 coroutines should carry the model, including for a future IO scheduler; the analysis went the other way and the user approved the slot model.

**Three questions you should be able to answer**
- Why does the parked slot carry a generation beside the channel value, and what breaks first without it — a reply to a dead client, or a reply to the wrong client?
- virtio-blk's `start()` completes inline; the library's async path is proven only by host tests. Which kernel gap stands between them, and what would the driver's completion look like once it closes?
- The slot table is the seat of a future IO scheduler. Which two insertion points does the design name, and why does a coroutine model have trouble interposing there?
