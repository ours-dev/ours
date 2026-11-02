# server

The plumbing every protocol server shares, so a new server starts at its
methods: the client set, the message pump, and the reply and epitaph helpers,
on a Loop.

## What the base handles

`server::ProtocolServer<Derived, MAX_CLIENTS>` (CRTP, no virtuals) owns:

- the fixed client table: an Open of the protocol takes a free entry and arms
  a wait on the channel; a full table answers `NoResource` epitaph;
- the message pump: one `channel_read` per signal, header decode, and the
  call into the derived server's `on_message()`;
- `reply()` / `close_with_epitaph()` / `close_handles()` / `close_client()`,
  the handle hygiene a refused or ending connection needs.

The derived server implements one function, `on_message(channel, header,
message) -> loop::Action`, and returns `Rearm` (connection stays) or `Close`
(it ends). One `on_message` runs to completion before the next: a server may
run its method's work inline, and slot or client state needs no lock.

## The deferred-reply pattern

A reply does not have to go out inside the `on_message` that received the
request. Park it: copy what you still need — the channel, the txid, the
payload — out of the shared buffers into storage of your own, and build the
reply later, in a later callback on the same Loop, writing it with `reply()`
as usual. Two rules hold the model together:

- The shared request/reply buffers are transient within one callback. The
  next `channel_read` overwrites them, so anything a parked request still
  needs has been copied out by then.
- A reply that fails because the client went away while its request was
  parked is dropped silently; the client is gone either way.

The block-server library (`//system/lib/block-server`) is the first server
built on this pattern: its slots park requests for the hardware's completion
interrupt.

## Layout

```text
include/server/          public headers, reached as <server/server.hpp>
```

## Building

The library is `//system/lib/server`, header-only
(`cpp_interface("server")`). Consumers: `//system/driver/virtio-blk`, and
the block-server library beside it.
