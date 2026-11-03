# block-server outstanding repairs

## 1. A completion that ends a connection does not end it: `close_client`'s Close is discarded, and the freed entry can be reused while its slot is still armed

**Severity**
High: cross-wired connections, reachable today — a client that exits while its Read is parked makes `reply()` fail, and the inline virtio completion runs `complete()` from inside that client's own pump callback.

**Symptom**
Latent until a client disconnects mid-request, then wrong: the entry is freed while the client's Loop slot is still armed on the dead channel. The next Open reuses the entry (`connect()` takes any entry whose channel is Invalid), bumping its generation — and now one `Client` Waiter is the registered waiter of two live slots. The dead channel's `PeerClosed` packet then fires the old slot, whose `serve_client` runs on the reused entry: at best it closes client B's live connection; at worst it reads B's queued message and answers it with A's parked txid.

**Cause**
`complete()` (`system/lib/block-server/include/block_server/block_server.hpp:169-173` and the Read-contract path at `:164`) ends the failing connection by calling `Base::close_client(slot.channel)` — but `close_client` (`system/lib/server/include/server/server.hpp:147-156`) only clears the entry's channel and **returns** `loop::Action::Close` for the enclosing callback to propagate. `complete()` is `void`, called from the driver's completion path, and nothing propagates: the connection does not end. The Loop's rule is that a slot's wait ends only through its own Waiter's answer (I3), and a Waiter may not touch another slot — `complete()` sits outside the pump, where the pump's convention has no force.

**Change**
Two shapes, to be decided with the block round's design:
- **Mark, don't free.** `complete()` marks the entry dead (a flag the pump checks) instead of freeing it; the slot's own `PeerClosed` packet then runs the normal `serve_client` → `release` path, which propagates Close properly. One field; keeps every Loop invariant. The entry is reclaimed by the disconnect that is already in flight.
- **A base-level `close_connection(Client&)`** that closes the channel handle (which cancels the armed wait without a packet, per the kernel's close semantics) — but the Loop slot then never fires and is leaked; the Loop would need a slot-reclaim verb it does not have. More moving parts; recorded as the alternative.

The first is recommended. The same fix covers `irq::take`-style completions once the interrupt-to-Loop capability lands (`docs/repairs/261103/kernel-interrupts.md`).

**Reference**
`loop::Loop`'s I3 invariant (`system/lib/loop/include/loop/loop.hpp`, "a slot's handle is closed only in step 4"); Zircon's channel close cancels armed waits exactly as ours does.

**Acceptance**
A host test: park a Read, close the client, run the completion, then connect a second client and drive both — the second client's entry must not be the first's, and the first's slot must retire without serving the second's channel. Until it lands, the disconnect-mid-request shape must not be exercised by any test (the suite currently does not: `AClientGoneWhileParkedDropsItsReply` passes because the completion drops the reply before any reuse).
