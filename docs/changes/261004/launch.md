# launch changes

## Title: The launcher service's protocol and client, in the launch library (launcher-service step 1) (2026-10-04)

**Files**
- `system/lib/launch/include/launch/protocol.hpp`, `src/protocol.cpp` — new: the Launch and Connect requests, the Reply, `encode_launch()`, `decode_launch()`, `ordinal_of()`; a target of its own (`:protocol`) that builds for the host too
- `system/lib/launch/include/launch/client.hpp`, `src/client.cpp` — new: `launch::Client`, with `launch()` and `connect()`
- `system/lib/launch/BUILD.gn`, `test/BUILD.gn`, `test/protocol_test.cpp` — the targets and three host tests
- `.agents/tasks/launcher-service/DESIGN.md`, `PLAN.md` — the design worked out, the two open questions settled, the plan

**Symptom**
None, a planned change: step 1 of `.agents/tasks/launcher-service/PLAN.md`. A program that wanted to start another had to hold VMEX itself (init and the shell both did); the launcher service needs a way to be asked, and nothing uses it yet.

**Cause**
The format is the part worth learning, because it reuses one that exists.

The objects:
- **A processargs message** (`<ours/process_args.hpp>`): what a started program reads from its bootstrap channel. It holds a header, one handle_info per handle, then args, environ and names as NUL-terminated strings. It has one encoder, `encode_process_args()`, and one decoder, `ort::ProcessArgs::parse()`, which checks every offset and string against the message.
- **A Launch request**: a small header (the ordinal, the BootFS name's length), the name, padding to 4 bytes, and then a processargs message exactly as the child should get it, minus VmaRoot. Its handles are the job, then the child's startup handles, in handle_info order.

The events, for one launch:
1. The client encodes the request: the header and name by hand, the rest with `encode_process_args()`.
2. The launcher checks it with `decode_launch()`: the header and name by hand, the rest with `ort::ProcessArgs::parse()` for one handle fewer than arrived (the job). It refuses a VmaRoot among the handle_info, since that one is the launcher's to add.
3. The launcher starts the program, appends VmaRoot, and writes the processargs on.

So the protocol needs no string or handle layout of its own: the child's processargs is the request's body, and checking the request is checking what the child would read. A Launch therefore always fits one message, since the child's processargs had to fit one anyway. That was the argument for this over Fuchsia's sequence of Add* calls (DESIGN.md, the request-format decision).

**Change**
`decode_launch()`, the half that reads what an untrusted program sent:

```cpp
if (header.ordinal != Ordinal::Launch || header.name_size == 0 ||
    header.name_size > MAX_PROGRAM_NAME) {
    return Status::InvArgs;
}
auto const offset = process_args_offset(header.name_size);
if (offset > message.size() || num_handles == 0) {
    return Status::InvArgs;
}
ort::ProcessArgs parsed;
if (ort::ProcessArgs::parse(message.subspan(offset), num_handles - 1, &parsed) != Status::Ok) {
    return Status::InvArgs;
}
// ...and no handle_info names VmaRoot
```

`Client::launch()` writes the request with a duplicate of the job, so the caller keeps its own, then waits for the reply on the same channel. Every handle given to it is consumed whatever the answer, as `channel_write` consumes them. `Client::connect()` sends a channel end in a Connect request and hands back the other end. The limit is named `MAX_PROGRAM_NAME`, not `NAME_MAX`, which is a macro in a host `<limits.h>`; the host build said so.

**Reference**
- `fuchsia.process.Launcher` (`sdk/fidl/fuchsia.process/launcher.fidl`): AddArgs, AddEnvirons, AddNames and AddHandles, then Launch. Here the five are one message.
- Zircon's `processargs_read()` and `ld::ProcessargsBuffer`, the decoders `ort::ProcessArgs::parse()` is stricter than.

**Verification**
- Host tests (`system/lib/launch/test:protocol`), 3 passed:
  - a round trip;
  - encode refusals: an empty or over-long name, a VmaRoot, too many handles, a buffer too small;
  - decode refusals: no job, a wrong handle count, every truncation of a good message, a wrong ordinal, a zero, over-long or huge name length, and a VmaRoot forged into the handle_info.
- `boot_check.py`: dbg PASS (24), rel PASS (21). Nothing calls the new code yet; the build compiles the client into the user programs that link the library.
- clang-tidy over both new sources: nothing reported.

**Deliberately not done**
- The launcher program and every caller: steps 2 and 3.
- A test of `Client` itself, which needs a launcher on the other end; step 2 exercises it at every boot.

**How it was found**
None.

**The user's prediction**
None. The user asked to go straight to the work, so the request format and the reply were settled as recommended (DESIGN.md).

**Three questions you should be able to answer**
- Why can a Launch request never be too large to send, when the program it starts gets its arguments the same way?
- Why does `decode_launch()` parse the processargs for one handle fewer than the message carried, and refuse a VmaRoot?
- Why does `Client::launch()` duplicate the job but consume every other handle?

## Title: The launcher program, and init starting everything through it (launcher-service step 2) (2026-10-04)

**Files**
- `system/service/launcher/launcher.cpp`, `BUILD.gn` — new: `bin/launcher`
- `system/service/init/init.cpp`, `BUILD.gn` — init starts the launcher with the library (`start_launcher()`), then the shell and the autorun command with `launch::Client` requests; `start_then_write()` goes
- `kernel/products/image/dev/BUILD.gn` — the dev image packages the launcher
- `system/lib/launch/include/launch/protocol.hpp`, `src/protocol.cpp`, `client.hpp`, `test/protocol_test.cpp` — `MAX_STRINGS` (64 of each kind) becomes a protocol limit, refused by both the encoder and the decoder, instead of a constant only the launcher knew

**Symptom**
None, a planned change: step 2 of `.agents/tasks/launcher-service/PLAN.md`. Until now init started the shell and the autorun command itself, with the library and its own VMEX.

**Cause**
The launcher is a server, and its loop is the first program built on the port (K26 to K28). How it serves is worth learning.

The objects:
- **The port**, created at start. The launcher sleeps on nothing else.
- **A connection**: a channel, in a slot of a fixed table of 16. Each slot's index is the key its waits carry.
- **One wait per connection**: `object_wait_async` for Readable or PeerClosed. It is one-shot, so it is re-armed after every message.

The events, for one request:
1. A client writes a Launch on its connection. The channel turns Readable, the armed wait queues a packet whose key is the slot.
2. The launcher's `port_wait` returns the packet. It reads one message from that slot's channel and serves it: a Connect adds a slot, and anything else is answered as a Launch.
3. It re-arms the slot's wait. If a second message is already queued, the wait is level-triggered, so it queues a packet at once and the loop comes straight back.
4. A packet with PeerClosed and no Readable means the client is gone: the slot's channel is closed and the slot freed.

Serving a Launch encodes the child's processargs before starting the program, so every check that can fail is made first, and once the program has started only the write itself can fail. A program whose processargs never comes does not hang: when the launcher closes its end of the bootstrap channel, the program's runtime sees PeerClosed and exits with EXIT_START_FAILED. But it would have been started only to die, after the client was told it started.

**Change**
The loop in `main()`:

```cpp
for (;;) {
    ours::PortPacket packet{};
    ours::port_wait(port, ours::INSTANT_INFINITE, &packet);
    auto const slot = static_cast<usize>(packet.key);
    if (!!(packet.signal.observed & ours::Signals::Readable)) {
        serve_message(launcher, connections, slot);
        if (!connections.arm(slot)) {
            connections.drop(slot);
        }
        continue;
    }
    connections.drop(slot);  // PeerClosed, nothing to read
}
```

init now starts one program itself, the launcher: `start_launcher()` gives it init's connection's far end (User0), duplicates of the BootFS, VMEX and the vDSO, and its root region. The shell and the autorun command become `Client::launch()` calls with the same startup handles as before, minus VmaRoot, which the launcher adds. init still holds VMEX in this step, and the shell still receives a duplicate and launches its own commands; step 3 takes both away.

**Reference**
- Fuchsia's component_manager serving `fuchsia.process.Launcher` (`src/sys/lib/elf_runner/src/process_launcher.rs`).
- Zircon `async::Loop` and `async::Wait`, the pattern the port loop is: a one-shot wait per object, re-armed in the handler.

**Verification**
- `boot_check.py`: dbg PASS (24), rel PASS (21).
- The rel log, in order: "init: launched the launcher", "launcher: serving", "launcher: started bin/shell", "init: launched shell", "init: shell says "pong"".
- `out/autorun-launch` (rel, `bin/fault_probe+none`): "launcher: started bin/fault_probe" and fault_probe's own lines.
- clang-tidy over `launcher.cpp` and `init.cpp`: nothing reported.
- HEAD plus only these files, in a scratch copy: dbg PASS (24), rel PASS (21), and the same launcher lines.
- A review workflow (two reviewers, each finding put to a refuter) left two standing findings, both wording, both fixed:
  - the launcher's comments said a program that never gets its processargs waits for ever;
  - several of init's comments still described the old arrangement.
  Taken from its smaller notes: `MAX_STRINGS`, above. Refuted, so not done: limiting Connect to init's connection and refusing a non-channel in a Connect. No client in the tree can send either; holding a connection is the capability, and the cap of 16 connections is documented.

**Deliberately not done**
- Taking VMEX from init and the shell, and the shell's commands through the launcher: step 3.
- An event-loop library: the loop is this program's own until U10.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the launcher re-arm a connection's wait after every message, and why does that not lose a second message that arrived meanwhile?
- Why is the child's processargs encoded before the program is started?
- Which program does init still start itself, and why that one?
