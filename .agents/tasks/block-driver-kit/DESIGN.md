# block-driver-kit: the block-driver library an outside author writes a driver against

**Date**: 2026-11-03
**Status**: Proposed

## Problem

`//system/lib/server` gives a new protocol server the connection plumbing (client table, message pump, reply and epitaph helpers), but its `on_message` must answer synchronously. The virtio-blk driver works inside that limit only by polling the device inline in `submit()` — a loop of `interrupt_wait` up to 100 rounds inside the request path. An author bringing up an interrupt-driven device (NVMe's completion queues, xHCI's doorbells and events) has no place to put the completion: the base provides no way to park a request and reply later, and the one working example hides its hardware in a private `BlockServer` class inside `virtio_blk.cpp`, from which a second driver can copy nothing. The server base itself has no test and no README. Three developers wanting to write block drivers would fork three half-copies of the same machinery.

The userspace requirement that forces this now: the macro filesystem landed on a working block service, and the next devices (NVMe, AHCI, SCSI) are all interrupt-driven; the driver-development direction the user set — "a complete driver library so other devices can adapt to our kernel" — starts at the block layer.

## Goals and non-goals

Goals:
- A deferred-reply core: a request can be parked with its client and txid, and the reply written when the hardware completes, from a Loop callback on the same thread.
- A reusable `BlockServer` in a library: any block driver implements only its geometry and its hardware submit; connections, decode, dispatch, slot accounting, backpressure and completion replies are the library's.
- The virtio-blk driver migrates onto the library and switches from inline polling to interrupt-driven completion, with no behavior change observable from `routing_checks`.
- The server base and the block library have host tests, and the fake kernel the ioservice tests keep becomes a shared test target.

Non-goals:
- The general driver kit (startup skeleton, PCI/ECAM and IRQ and DMA helpers, driver BUILD.gn/manifest templates, a guide chapter) — the next round, once this API settles.
- An IO scheduler — this design only leaves its seat: the slot table between allocation and `start()` is where a queueing policy inserts, and the completion callback is where a `dispatch()` runs. No scheduler code is paid for in advance.
- C++20 coroutines as the completion model — rejected, see Alternatives.
- Pipelining on the client side (btreefs stays synchronous); the model's concurrency is proven by host tests, the target run proves the wiring.

## Design

The objects:
- `server::ProtocolServer<Derived, MAX_CLIENTS>` (`system/lib/server/include/server/server.hpp`) stays what it is: one `Client` entry per connection, one shared request/reply buffer pair, `on_message()` called run-to-completion on the Loop thread. The shared reply buffer is transient within one callback — a parked request copies its context out, the same discipline ioservice's LoopServer states for a parked Open.
- `block_server::Slot` — one in-flight hardware request: `{channel, txid, op, lba, count, reply bytes}`. The reply bytes are per-slot, because between a request's submission and its completion other callbacks run and would overwrite any shared buffer.
- `block_server::BlockServer<Derived, MAX_CLIENTS, MAX_INFLIGHT>`, a new library `//system/lib/block-server`, deriving from `ProtocolServer<BlockServer<...>, MAX_CLIENTS>` and itself implementing `on_message`:
  - `GET_INFO` answers inline, from the driver's `geometry()`.
  - `READ`/`WRITE` decode, take a free slot, copy the request into it, call the driver's `start(slot)`, and return `Rearm` — the request is now the hardware's. When every slot is busy the library answers the backpressure status and keeps the connection.
  - `CLOSE` and an unknown ordinal follow the base's existing policy (close the connection; an unknown ordinal closes without an epitaph).
- The driver implements two CRTP hooks (no virtuals, the base's standing style): `geometry()` and `start(slot)` — start reads the slot's op/lba/count, programs the hardware, and returns Ok once the work is offered.
- `complete(slot, status, data = {})` — the library's completion entry, callable from any callback on the Loop thread (the driver's IRQ waiter): it encodes the reply into the slot's own bytes (READ carries `data`, a span that may point straight into the driver's DMA mapping — `channel_write` copies; WRITE carries the status only), writes it to the slot's channel, frees the slot. A channel that died while the request was parked (client disconnected) makes the reply fail; the library drops the reply and frees the slot silently, which is the contract the base's `reply()` documents.
- The descriptor→slot mapping (virtio's used-ring index, NVMe's command id) is the driver's: hardware identifiers are hardware dialect, and the library never sees them.

The events, one READ through an interrupt-driven driver:
1. the client's Open lands in `connect()` — the base takes a Client entry, arms the wait;
2. a Read arrives; `on_message` decodes it, takes slot 3, stores `{channel, txid, Read, lba, count}`, calls `start(3)`;
3. `start` writes the descriptor chain into the available ring, notes `descriptor 17 → slot 3` in its own table, kicks the device, returns Ok; the library returns `Rearm` — the connection stays and the next message may arrive while the disk works;
4. the device fires the line; the driver's IRQ waiter (a `loop::Waiter` on the interrupt handle, or the interrupt bound to the Loop's port — decided at implementation) drains the used ring, finds 17, and calls `complete(3, Ok, span over the DMA region)`;
5. the library encodes the reply into slot 3's bytes, writes it to the parked channel, frees the slot.

Backpressure and budget: `MAX_INFLIGHT` is the tag depth the hardware can take; `MAX_CLIENTS` stays the connection bound. Both are template parameters so a driver sizes them to its device. The single-thread invariant from the base carries over unchanged: one callback runs at a time, so slot state needs no lock — the correctness rests on the run-to-completion model, as the base's header documents, and a future multi-queue device revisits it there.

The scheduler's seat: a future IO scheduler is a queueing policy between slot allocation and `start()` plus a `dispatch()` in the completion path (Linux blk-mq's `blk_mq_dispatch_rq_list` and its elevator callbacks are the reference shape; every scheduler there is a callback state machine). Nothing in this round is paid for it beyond the seam existing naturally.

Where the fake kernel goes: the ioservice host tests keep a fake kernel (channel_read/write, handle tables, the Loop's port) in their test directory. It moves to a shared testonly target (`//system/lib/testing/fake_kernel`), from which the ioservice tests and the new server/block-server suites both link.

## Alternatives considered

- **C++20 coroutines as the completion model.** Rejected. The matching table (descriptor/command-id → request) is needed under either model, so coroutines add a wrapper layer, not a simplification; user programs have no heap (`system/lib/ort/src/cpp_dependencies.cpp`), so coroutine frames would need a static pool — which is the slot table wearing a `coroutine_handle`; cancelling a parked coroutine on client disconnect is the dangling-frame classic, where the slot model frees one entry; and the reference systems keep coroutines out of driver completion paths — Zircon/Fuchsia serve FIDL with `.Then` callbacks, Linux blk-mq and every one of its schedulers are callback state machines. Coroutines remain a candidate for a later client-side async API (a pipelined `ours_block::Client`), decided separately; on the server side they would also lock the control flow to the initiator, which is exactly where a future IO scheduler needs to interpose.
- **Inline polling stays, library only extracts the class.** Rejected as the end state: it caps every future driver at virtio-blk's busy-wait shape and leaves the one genuinely hard problem unsolved. It is, however, the migration's fallback — if the interrupt-to-Loop wiring hits a wall, the library still serves with `start()` completing inline, and the async path is then proven by tests alone.
- **Async support pushed into `ProtocolServer` as a generic parking table.** Rejected for now: block is the only protocol that needs it, the table's fields (op, lba, count, data span) are protocol-shaped, and a generic version would guess wrong for the second user. The base instead gains the documented deferred-reply pattern; when a second protocol needs parking, the shared shape can be lifted then.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope of the round | block library deepened; general driver kit first; three phases at once | block library deepened — the deferred-reply core is what blocks real drivers | — | block library deepened (2026-11-03) |
| Completion model | slot table + callbacks; C++20 coroutines | slot table | user asked twice for coroutines, decided against after the trade-off | slot table (2026-11-03) |
| IO scheduler | design for it now; leave the seat | leave the seat — slot table is the insertion point | user proposed the scheduler concern | leave the seat (2026-11-03) |

## Testing and verification

- Host: the shared fake kernel keeps every ioservice test green; new suites cover the base's deferred-reply pattern and the block library's slot accounting (allocation, exhaustion backpressure, READ data integrity, WRITE status reply, disconnect-while-parked, epitaph paths). Existing 159 host tests stay green.
- Target: `boot_check.py` dbg and rel PASS, rel 7/7 required; disked, `routing_checks: passed` with the GPT suite actually running (the verdict must not print the not-run line).
- Source: clang-format and clang-tidy clean on every changed line (`cpp-write-and-verify`).
- Not verifiable yet: true pipelined concurrency on the target (the client is synchronous); real NVMe hardware.

## Open questions

- None. Both questions the plan left open are settled: the backpressure status is `ShouldWait`, which the table carries (status.hpp:71); and the interrupt-to-Loop wiring is settled the other way — no path delivers an interrupt into a Loop, a kernel capability gap filed as `docs/repairs/261103/kernel-interrupts.md`, with virtio-blk completing in `start()` under the design's stated fallback.
