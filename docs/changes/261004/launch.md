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

## Title: Only the launcher holds VMEX: init lets go of its own, and the shell starts its commands through a connection (launcher-service step 3) (2026-10-04)

**Files**
- `system/service/init/init.cpp` — after starting the launcher, init closes its VMEX and the executable BootFS made with it; `start_shell()` gives the shell a connection (Connect, `launch::protocol::CONNECTION`) in place of VMEX
- `examples/shell/shell.cpp`, `shell_launcher.hpp` — `Starter`: a connection, or the library where the shell holds VMEX; `launch_command()` uses whichever it has; `ls` reads a read-only mapping of the shell's own when there is no library
- `examples/shell/shell_tests.cpp`, `shell_tests.hpp` — `check_launcher_service()`
- `system/lib/launch/include/launch/bootfs.hpp`, `src/bootfs.cpp` — `map_bootfs()`, split out of `make_bootfs_launcher()`
- `system/service/launcher/launcher.cpp` — a launcher started without its handles exits instead of halting
- `examples/shell/BUILD.gn` — a comment on who starts the shell

**Symptom**
None, a planned change: step 3 of `.agents/tasks/launcher-service/PLAN.md`, and the user's decision of 2026-10-03 that the launcher be the only holder of VMEX. After step 2, init and the shell still held it.

**Cause**
VMEX is the capability to turn a VMO into one that may be executed (`vmo_replace_as_executable`). Whoever holds it can run any code it can write into a VMO. Design D3 meant init alone to hold it; the interactive shell's work had spread it to the shell too. With a launcher, a program that wants another program started no longer needs it at all: it asks.

The objects, in the dev image after boot:
- **userboot** had VMEX from the kernel, handed it to init, and exited.
- **init** used it once, to make the executable BootFS copy the library needs to start the launcher, and then closed both.
- **the launcher** holds the only VMEX handle, and the only executable BootFS.
- **the shell** holds a connection to the launcher, the BootFS without Execute (for `ls`), and the vDSO's VMO (for its vDSO-mapping check).

The events, for a command typed at the shell:
1. The shell resolves the name (`bin/<word>`) and sends a Launch on its connection, with a duplicate of its job and the words as argv.
2. The launcher starts the program and replies with the process handle.
3. The shell waits for the process to terminate, as before.

In the dbg_1 image there is no init and no launcher: userboot starts the shell, which holds VMEX and uses the library, as before. That image's shell is the first program after userboot, the place init holds in the dev image.

**Change**
init, right after the launcher is started:

```cpp
auto _ = ours::handle_close(library.executable);
auto _ = ours::handle_close(vmex);
auto _ = ort::debug_line("init: vmex is the launcher's alone");
```

The shell's command path:

```cpp
if (starter.connection != ours::Uandle::Invalid) {
    auto const started = launch::Client(starter.connection).launch(job, path, args, {}, {}, {});
    ...
} else {
    auto const started = start_with_library(starter.library, job, path, args);
    ...
}
```

`map_bootfs()` is the first half of what `make_bootfs_launcher()` did: it reads the header through the VMO and maps the BootFS read-only. A shell with no library calls it for `ls`.

**Reference**
- Fuchsia: programs start others through `fdio_spawn()` and `fuchsia.process.Launcher`. There, VMEX (`ZX_RSRC_SYSTEM_VMEX_BASE`) is held by component_manager, which makes BootFS programs executable as it hands them out, and the launcher never holds it. Here the launcher holds it, because clients name BootFS files rather than bring executable VMOs (DESIGN.md).

**Verification**
- `boot_check.py`: dbg PASS (24 passed, 1 skipped: the launcher service checks, since dbg_1 has no launcher), rel PASS (22 passed, 3 skipped: the three checks that need VMEX, which the dev shell no longer has).
- The rel log: "init: vmex is the launcher's alone", "launcher: a new connection" (the shell's), "launcher: started bin/shell", "shell: started without vmex", "shell: launcher service checks passed".
- `check_launcher_service()`:
  - a name the BootFS lacks is NotFound;
  - a malformed Launch (no name, no job) is answered InvArgs, and the next request on the same connection is served;
  - a second connection from Connect serves requests too;
  - the shell holds no VMEX.
- Typed at the dev shell over serial, on the autorun build:
  - `ls` lists the BootFS through the shell's own mapping, including `bin/launcher`;
  - `fault_probe none` is started by the launcher ("launcher: started bin/fault_probe") and runs;
  - `nosuch` answers NotFound.
- clang-tidy over the three changed sources: nothing reported.
- Typing `launcher` at the dev shell: the stray launcher says it has no handles and exits, and the shell's prompt comes back (`ls` then works). Before, it halted, and the shell waited for it for ever.
- HEAD plus only these files, in a scratch copy: dbg PASS (24, 1 skipped), rel PASS (22, 3 skipped).
- A review workflow (two reviewers, each finding put to a refuter) left no standing finding. From its smaller notes:
  - `map_bootfs()`'s failure is reported instead of dropped;
  - the malformed-request check waits for PeerClosed too, so a launcher it killed fails the check instead of hanging the boot;
  - a stray `launcher` exits (above);
  - four comments that still described the old arrangement are corrected.
  Refuted, so not done: splitting the shell's VMEX-gated checks so the BootFS-only parts run in the dev image too. Step 3 did not change those gates, and dbg_1 runs the same checks on a BootFS handle with the same rights.

**Deliberately not done**
- init's own BootFS mapping stays (no `vma_unmap`); it is read-only.
- init's startup list (`config/init/startup`) is still only packaged: init starts the shell by name, as before.
- The dbg_1 image keeps its shell holding VMEX, as above.
- A typed `init` still halts and holds the shell: init halts when started without the root job. That predates this work.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- After boot of the dev image, which process holds VMEX, and what did each of the others do with the handle it had?
- Why does the dbg_1 image's shell still hold VMEX?
- Why does the shell map the BootFS itself for `ls` now?

## Title: A launcher reply is never a request, and a connection that sends no request is closed (2026-10-04)

**Files**
- `system/lib/launch/include/launch/protocol.hpp` — `Ordinal::LaunchReply` (`0x8000'0001`), the reply's own ordinal
- `system/lib/launch/src/client.cpp` — the client expects it
- `system/service/launcher/launcher.cpp` — `serve_message()` answers only a Launch; anything else but a well-formed Connect closes the connection
- `examples/shell/shell_tests.cpp` — a reply sent back as a request closes the connection

**Symptom**
Found by the VFS design research, not seen running. A client could make the launcher spin for ever and hold two of its 16 connection slots after the client itself had gone.

**Cause**
The objects:
- A Reply was `{Ordinal::Launch, status}`: 8 bytes, which is also a valid Header `{Launch, name_size = status}`.
- The launcher answered every message that was not a Connect, as a Launch: a malformed one with an InvArgs Reply.
- A Connect accepted any channel end, so a client could hand the launcher both ends of one channel.

The events:
1. The client makes a channel (A, B), writes one byte of garbage into A (so it waits on B), then sends Connect(B) and Connect(A).
2. The launcher reads B's garbage and answers InvArgs on B. The answer arrives at A.
3. It reads A's message, which is its own Reply, decodes it as a Launch whose name runs off the message, and answers InvArgs on A. That answer arrives at B.
4. Back to step 2, for ever. The launcher holds both ends, so PeerClosed never comes and the slots are never freed.

The plan's wire format (`docs/design/260924/userspace-filesystem.md`, section 2.1) has a direction field for exactly this reason: a message from the server must not be mistakable for one to it.

**Change**
- The Reply carries `Ordinal::LaunchReply`, top bit set, so it is never a request.
- The launcher answers a Launch, malformed or not, since the client is waiting. It closes a connection that sends a bad Connect or any other ordinal, as the planned format closes one on an unknown ordinal, rather than answering it.

```cpp
if (ordinal.is_none() || *ordinal != protocol::Ordinal::Launch) {
    close_all(carried);
    return false;  // the loop drops the connection
}
```

In the scenario above, step 3 now reads a LaunchReply and closes A, and B's peer is gone.

**Reference**
The planned `MessageHeader.direction` and its rule, the server rejects ToClient (`docs/design/260924/userspace-filesystem.md`, section 2.1); FIDL's message header, `fidl_message_header_t`, whose flags and ordinal space play the same part.

**Verification**
- Host tests: 3 passed.
- `boot_check.py`: dbg PASS (24, 1 skipped), rel PASS (22, 3 skipped).
- The rel log: "launcher: closed a connection that sent no request" when the shell sends a LaunchReply on its second connection, and the shell sees that connection's PeerClosed.
- Not run: the two-ended loop itself. It needs a client that hands the launcher both ends, which nothing in the tree is.

**Deliberately not done**
- Moving the launcher protocol onto the planned wire header (txid, direction, magic), which waits for the VFS design's decision on the protocol generator.
- A per-client connection limit: one client can still fill the 16 slots with Connects whose peers it keeps.

**How it was found**
The VFS design research (old-plan report, item 23): the plan's argument for the direction field applied to the launcher's protocol, which has none.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did a reply that decoded as a request let a client make the launcher loop?
- Why is a malformed Launch still answered, while an unknown ordinal closes the connection?
- What does the planned wire format's direction field do that this ordinal now does?
