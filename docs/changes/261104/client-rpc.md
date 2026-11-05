# client-rpc changes

## Title: the call pipeline as an object, under every typed client (2026-11-04)

**Files**
- `system/lib/rpc/**` — the core: `rpc::conduct` (the free pipeline: send, wait, read; handles intact, epitaphs untranslated; caller-owned reply buffers) and `rpc::Channel` (ownership + txid + `call()`, the translation wrapper: stray handles closed, epitaph-as-failure). User-only (it makes syscalls); its header compiles on the host, where its fake-kernel suite runs.
- `system/lib/protocols/include/prot/{block,dev}_client.hpp` — the pasted pipelines deleted; the clients hold `rpc::Channel` and typed methods only.
- `system/lib/protocols/src/{resource,acpi}_client.cpp` — conducted through `rpc::conduct` with the families' own verdicts kept: the resource reply's handle **is** the payload (call() would have closed it), acpi refuses a handle-bearing reply and translates epitaphs its contract's way.
- `system/lib/protocols/include/prot/fshost_client.hpp` — new: `mount(directory, name)`, the first client for the Admin protocol.
- `system/service/init/test/routing_checks/**` — check_block, check_nvme and the mount check speak the typed clients; the mixed-abstraction note closed.

**Symptom**
None — a consolidation. The plumbing existed in as many copies as there were
typed clients, plus a fourth paste in routing_checks' own `call()` helper.

**Cause**
The typed-client pattern grew one paste at a time. The rule now written
down: a client's author writes typed methods and nothing else; the pipeline
is rpc's.

**Change**
The core's two layers, and the boundary the round taught:

```cpp
// the free pipeline: caller-owned reply buffers, handles intact
auto const message = rpc::conduct(connection, request, reply_bytes, reply_handles);
// the translation wrapper: strays closed, epitaph-as-failure
auto answered = m_channel.call(request);
```

The shape lesson: **the pipeline cannot return a view of its own stack**.
The first conduct returned its local buffer's view; the host suites passed
on dead-frame luck and the dual-disk boot failed fourteen checks deep --
every resource consumer -- because the resource reply's handle is the
payload and the view was gone. Caller-owned buffers are the fix, and the
reason steps verify on target before they land.

**Reference**
- `rpc::conduct`/`Channel::call` vs the pasted `receive()` it replaces:
  same bytes, one home. Fuchsia's counterpart is the FIDL bindings'
  synchronous call path -- ours is the hand-written stage of that path,
  per the user's sequencing (the IDL/generator deferred; this is what it
  would emit).
- The clients' semantics unchanged: block's read/write/get_info, dev's
  publish, resource/acpi verdicts -- each pinned by its suite where one
  exists.

**Verification**
- Host: rpc 5 (round trip, epitaph-as-failure, silent peer reads
  PeerClosed, the txid wrap, a stray handle closed), registry 9, dev 8 --
  all green through the real core.
- Dual-disk on target: dbg 46 / rel 44, both leaves answering, the mount
  through the typed fshost client, the GPT magic and the NVMe scratch
  round trip through the block client, `routing_checks: passed`.

**Deliberately not done**
- process.Launcher: launch's ProcessLauncherState is the server-side
  conductor init drives, not client plumbing; it stays.
- io: fsio already is the io client framework.
- echo and the probe-only protocols: no client, by design -- the suite
  tests them at the wire deliberately.

**How it was found**
The resource-consumer failures (14 checks deep) were the dangling-view
bug announcing itself; the log's shape (every resource consumer, InvArgs)
named the funnel, and the diff named the shape. Recorded under Change.

**The user's prediction**
None -- the user chose the round's shape (A now, IDL later).

**Three questions you should be able to answer**
- Why does resource::get conduct instead of call? (What is the reply's
  payload, and what would call() have done to it?)
- What does rpc::conduct return when the provider queued an epitaph and
  died, and who decides the verdict?
- What did the round's first conduct shape get wrong about stack lifetime,
  and which suite failed to catch it on the host?
