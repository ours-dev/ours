# client-rpc: one call pipeline under the typed clients, none hand-pasted

**Date**: 2026-11-04
**Status**: Proposed

## Problem

The two-way call plumbing a protocol client needs -- the txid counter, the
two message buffers, the send, the wait, the read, the epitaph translation --
exists in as many copies as there are typed clients: prot::block::Client,
prot::resource's and prot::acpi's helpers, and a fourth paste in
routing_checks' own `call()` helper, which its check_block/check_nvme use
beside the typed clients the same file already holds (the mixed-abstraction
note the protocols round's audit recorded). Two families (fshost, process)
have no client at all, so their consumers speak raw frames.

## Goals and non-goals

Goals:

- One `rpc::Channel` -- the call pipeline as an object: takes a channel,
  sends, waits, reads, translates an epitaph into the failure, counts txids.
  Bytes and statuses alone; no family knows it.
- The typed clients (block, resource, acpi, dev) keep their public APIs and
  conduct their calls through it. Their hand-pasted plumbing goes.
- A fshost client, so the mount check speaks typed methods.
- routing_checks' raw-frame calls move onto the typed clients; its local
  `call()` helper goes.

Non-goals:

- process.Launcher: launch's ProcessLauncherState is the server-side
  conductor init drives, not client plumbing; it stays.
- io: fsio already is the io client framework; nothing to add.
- No wire bytes change; the codecs are untouched.

## Design

The objects:

- `//system/lib/rpc` (`include/rpc/rpc.hpp`, namespace `rpc`), header-only,
  user toolchain only (it makes system calls): `rpc::Channel`, constructed
  over a Uandle it takes ownership of (closing it on death, as the clients
  do today), with `call(request) -> Result<Message, Status>` (send, wait
  Readable|PeerClosed, read, close stray handles, translate an epitaph into
  the failure status) and `next_txid()`. The buffers are its members, so
  one-at-a-time per channel remains the contract the callers keep.
- The typed clients hold an rpc::Channel instead of a raw Uandle and their
  own buffer/txid members; `publish()`/`get()`/`read()` keep their shapes.
- `prot::fshost::Client` -- the Mount call typed, over the same core.
- routing_checks: check_block/check_nvme conduct GetInfo/Read/Write through
  prot::block::Client; the mount check through prot::fshost::Client. The
  wire-level probes that exist to test refusals (a malformed body, a wrong
  txid) stay raw -- they are testing the wire, not using it.

## Alternatives considered

- **Put the core in wire** -- wire is bytes-only by contract and builds on
  the host; a syscalling core would break that. A separate library keeps
  wire host-clean.
- **Generate the clients** (the IDL path) -- deliberately deferred; this
  core is what a future generator emits calls into, the hand-written
  prototype of the generated shape (the user's chosen sequencing).

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Round shape | A (rpc core + clients) now, IDL later / IDL now / status quo | A now; the core is the prototype of what a generator would emit | — | user chose A 2026-11-04 |
| Core home | its own lib (rpc) / inside wire | own lib (wire stays bytes-only, host-clean) | — | — |
| Launcher client | add one / leave launch's conductor | leave (server-side state machine, not client plumbing) | — | — |

## Testing and verification

- The rpc core's host suite over the fake kernel (the registry test's
  harness shape): call round trip, epitaph translation, peer-closed, the
  txid's wrap, a reply's stray handles closed.
- The existing protocol suites stay green (the clients' public APIs
  unchanged); routing_checks passes with its raw-frame calls gone; full
  boot_check, dual-disk.

## Open questions

None.
