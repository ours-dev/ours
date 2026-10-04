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

## Title: The fuchsia.process.Launcher and fuchsia.kernel.DebugResource codecs, and the Launcher's per-connection state (component-framework step 6) (2026-10-04)

**Files**
- `system/lib/fuchsia-process/` — new library, scaffolded with copier from `tools/templates/cpp_library`: the methods `ADD_ARGS`, `ADD_HANDLES`, `ADD_NAMES` and `LAUNCH`, `LAUNCHER_NAME`, `MAX_NAME_LEN`, `MAX_PATH_LENGTH`, the structs `HandleInfo`, `NameInfo`, `LaunchInfo`, `PackedStrings` and `StringReader`, and an encoder and a decoder for every request and for Launch's reply
- `system/lib/fuchsia-process/test/fuchsia_process_test.cpp` — 21 host tests: a round trip for every message, every malformed form refused
- `system/lib/fuchsia-kernel/` — new library, scaffolded with copier: `DEBUG_RESOURCE_GET`, `DEBUG_RESOURCE_NAME`, and the encoders and decoders of Get and its reply
- `system/lib/fuchsia-kernel/test/fuchsia_kernel_test.cpp` — 5 host tests
- `system/lib/launch/include/launch/process_launcher.hpp`, `system/lib/launch/src/process_launcher.cpp` — `ProcessLauncherState`, the Launcher's per-connection state, with `LauncherOps` (the start and close hooks), `Startup` and `LAUNCHER_HANDLE_INFO`
- `system/lib/launch/test/process_launcher_test.cpp` — 14 host tests, the processargs read back with `ort::ProcessArgs::parse()`
- `system/lib/launch/BUILD.gn`, `system/lib/launch/test/BUILD.gn`, `system/lib/launch/README.md` — the `process_launcher` source set, its test and its line in the README
- `system/lib/wire/test/ordinals_test.py` — the five new selectors join the list the scan must find
- `BUILD.gn` — `//system/lib/fuchsia-process:tests` and `//system/lib/fuchsia-kernel:tests` join the default build

**Symptom**
None; this is step 6 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Nothing serves or calls the Launcher yet: the root's Launcher server and fsio's spawn both arrive in step 14, and the shell's use of DebugResource in step 15. The old name-based protocol (`launch/protocol.hpp`) stays until step 16.

**Cause**
Why the Launcher keeps state between messages, and what that costs a root with no heap. The objects:
- the client: a component that wants to start a program, such as the shell running `x a b`;
- the connection: one channel from the client to the root's Launcher;
- the per-connection state: what the client has added so far;
- the processargs: the one message a new process is started with (`<ours/process_args.hpp>`), a 36-byte header, 4 bytes of handle_info per handle, then the strings, all in one channel message of at most 4096 bytes and 64 handles.

The events of one launch, as fdio_spawn sends them:
1. AddArgs `["x", "a", "b"]`, AddHandles with a duplicate of the job as JobDefault, AddNames with a clone of each namespace entry. All three are one-way: no reply, any number of times, in any order, each one appending.
2. Launch with the program's VMO, the job and the process name, two-way.
3. The Launcher builds the processargs from everything appended, adds its own two handles (the new process's VmaRoot and the vDSO's VMO, design D19), starts the program, answers `{Ok, process}`, and forgets everything: the connection is as new.

Why stateful at all (D13). One message could carry a whole launch, as the old protocol's did (D13 b): the program and the job, plus 62 startup handles, make exactly the 64 a message holds. D13 chose Fuchsia's shape instead, the protocol fdio_spawn speaks: a client assembles a launch from several places (fdio_spawn adds the job in one function, the namespace in another, the arguments in a third), each Add* message carries only its own part, and the same client code would run against Fuchsia's Launcher. The price is state on the server between messages. Fuchsia's `ProcessLauncherState` is four `Vec`s that grow as the client sends. The root has no heap (user programs have none, `system/lib/ort/src/cpp_dependencies.cpp`), so ours is fixed in size. The sizes are not arbitrary; each one is what a processargs can carry:
- 62 handles: a processargs is one channel message, so it carries at most 64 handles, and the Launcher adds 2.
- 4052 bytes of strings: 4096, less the 36-byte header, less the 8 bytes of the Launcher's two handle_info. Every handle the client adds takes 4 more bytes out of the same message, so the true limit on strings depends on how many handles there are, and only Launch, which sees both, can judge it.
- 64 arguments: this one is the state's own, not the protocol's. The state keeps where each string lies as a 4-byte `{offset, size}` piece in a fixed array, and `encode_process_args()` wants the strings as an array of views, which `start()` lays out on its stack.

That is 5120 bytes per connection (`sizeof(ProcessLauncherState)` on x86-64), against D13's estimate of "about 4.6 KiB": the 4052-byte arena, 62 handles and 62 handle_info (496 bytes), and 126 pieces (504 bytes) for the arguments and the paths. D27's four Launcher connections are 20 KiB of the root's BSS.

Why an Add* that does not fit sets a mark instead of being refused on the spot. Add* is one-way, so there is no reply to carry a refusal. The only other signal is closing the connection, which the client cannot tell apart from the Launcher crashing. Fuchsia answers at Launch: a processargs with more than 64 handles or more than a message's bytes is `ProcessargsError::TooManyHandles` or `SizeTooLarge`, which `as_zx_status()` maps to INVALID_ARGS. So the state sets a mark, the Launch after it answers InvArgs without starting anything, and the client learns of it exactly where it would on Fuchsia. The handles of the Add* that did not fit are closed at once, since there is no room to keep them until Launch.

Why some handle types are refused. The Launcher adds some handles itself, and others describe objects only the Launcher creates for the new process:
- VmaRoot is the new process's root region. A second one from the client would leave the program's runtime two VmaRoot handles to choose from, and the client's would be a region of some other process, so the child would map its stack or heap into a process that is not its own.
- ProcSelf, ThreadSelf, VmaLoaded, VmoStack and VmoExecutable likewise name objects of the process being built; from a client they would name another process's objects under a type that says "your own".
- NsDir carries an index into the processargs' names as its argument, and only AddNames brings the path that index must point at. Through AddHandles a client would pick the index itself: another entry's path, so that two directories claim one path, or one past the end.

Fuchsia's `ProcessBuilder::add_handles()` refuses exactly these with InvalidArg, which the Launcher answers as INVALID_ARGS. It also checks every handle of the call before adding any ("Intentionally separate from validation so that we don't partially add"), and so does `add_handles()` here: one refused entry closes every handle of that message. A handle_info with any of its reserved bits 8-15 set is refused too, as `HandleInfo::try_from()` refuses it in Fuchsia and `encode_process_args()` would at Launch.

Why the state owns every handle a message brings. Once the root has read a message, its handles sit in the root's table and nobody else will ever close them. A leaked channel end keeps its peer from ever seeing PEER_CLOSED; a leaked job keeps the job alive; and the table slot stays used for as long as the root lives, which is as long as the system. So `receive()` takes every handle of every message, and each one leaves exactly once: handed to the start hook or closed through `LauncherOps::close`. The process an Ok reply carries is not one of them; it comes from the hook, and the caller sends it. The tests check this for every case with one helper, `expect_accounted()`.

Why the state resets after every Launch, failed or not. Fuchsia's `serve()` writes `state = ProcessLauncherState::default()` after both the success and the failure arm. If a failed Launch kept the state, a client that retried would send its arguments a second time and the program would get them twice; and a mark, once set, would fail every Launch for the rest of the connection.

Why a malformed message closes the connection without an epitaph. Fuchsia's `serve()` reads with `stream.try_next().await?`, so a decode error returns from `serve()` and drops the channel. A peer whose bytes do not decode does not speak the protocol, and answering it would mean trusting its txid. Launcher is a closed protocol, so an ordinal it does not have closes the connection too, and a reply sent back as a request is refused by the direction byte, which is what keeps a client that hands the root both ends of one channel from making it answer itself (design invariant I5).

Why the start is a hook. Starting a program takes system calls: a channel pair, `launch::start()`, and the channel_write of the processargs. The state calls them through `LauncherOps`, a function pointer and its argument, as `elfload::Diagnostics` does. The state is then the same code on the host as in the root, and the tests hand it a fake that records what it is given, so they can read the processargs the program would get with the runtime's own decoder.

Why strings travel packed. AddArgs and AddNames carry `count` strings, each ended by a NUL, back to back. That is how processargs lays out its own string sections, so the state copies them into its arena as they arrive. FIDL sends each string as a `vector<uint8>`, which may hold a NUL, and Fuchsia's Launcher answers INVALID_ARGS at Launch for one that does ("Argument contained null byte"); here a NUL would end the string early, so the encoder refuses a string that holds one, and the decoder refuses a count that disagrees with the NULs. The process name is not packed, so a NUL in it decodes, and Launch answers InvArgs, as Fuchsia does ("Process name contained null byte").

Why DebugResource's reply has a status. Every reply in ours's framing has a status at offset 16 (`<wire/wire.hpp>`); Fuchsia's Get returns only the resource, so there a server that cannot produce one can only close the channel. Here it can answer the 24-byte status form, NoResource for instance, and the shell can say why.

**Change**
The layouts, offsets counted from the start of the message:

```text
Launcher.AddArgs     one-way  u32 count@16, u32 size@20, u8 args[size]@24
Launcher.AddHandles  one-way  u32 count@16, u32 0@20, {u32 handle, u32 id}[count]@24
Launcher.AddNames    one-way  u32 count@16, u32 size@20, u32 directory[count]@24,
                              u8 paths[size]@24+4*count
Launcher.Launch      two-way  u32 executable@16, u32 job@20, u32 name_size@24, char name[]@28
  Ok reply                    u32 status@16, u32 process@20
DebugResource.Get    two-way  (the header alone)
  Ok reply                    u32 status@16, u32 resource@20
```

The state's interface is one verb for the server and one for a connection that goes away:

```cpp
struct LauncherOps {
    auto (*start)(void *context, Startup const &startup) -> ustl::Result<ours::Uandle, ours::Status>;
    auto (*close)(void *context, ours::Uandle handle) -> void;
    void *context;
};

class ProcessLauncherState final {
  public:
    enum class Outcome : u8 { Continue, Reply, Close };
    auto receive(wire::Message message, std::span<u8> scratch) -> Received;
    auto reset() -> void;
    ...
};
```

A Launch is judged in this order: a mark set by an earlier Add* or a NUL in the name answers InvArgs; then the processargs is encoded into `scratch` by `ours::encode_process_args()`, the one encoder, whose OutOfRange (the strings fit the state but not beside the handle_info) becomes InvArgs; only then is the hook called, so nothing is ever started that could not be handed its processargs:

```cpp
auto const status = ours::encode_process_args(
    scratch, std::span{handle_info}.first(m_num_handles + LAUNCHER_HANDLE_INFO.size()),
    std::span{args}.first(m_num_args), {}, std::span{names}.first(m_num_names), &length);
if (status == Status::OutOfRange) {
    return ustl::err(Status::InvArgs);
}
...
auto started = m_ops.start(m_ops.context, startup);
// The hook took them, whatever it answered.
m_num_handles = 0;
```

Rejected:
- Judging handle types at Launch, as Fuchsia does: the client sees the same InvArgs, and judging at AddHandles lets the refused handles be closed at once instead of taking slots until Launch.
- Closing the connection on an Add* that does not fit: the client could not tell it from a crash, and Fuchsia answers at Launch.
- Keeping the strings as `std::string_view`s, 16 bytes each, instead of 4-byte pieces: 1.5 KiB more per connection.
- An arena with the arguments growing from one end and the paths from the other, to drop the argument limit: `encode_process_args()` takes the strings as arrays of views, so a bound on the array `start()` lays out remains either way.
- A second processargs encoder that copies the packed strings straight in: `<ours/process_args.hpp>` keeps one encoder for the whole system, and a second would be a second place for the format to go wrong.
- `receive()` returning the Launch for the server to start and then reset: two more calls the server could forget, where the hook keeps the reset and the handle accounting in one place.
- Moving the "status plus one handle" reply, which GetBackingMemory, Launch and Get now all have, into `wire`: it would change step 3's library while steps 4 and 5 build on it in parallel; the three copies are small, and the move is left for later.

Library names, an open question of the design closed here: `system/lib/fuchsia-process` and `system/lib/fuchsia-kernel`, as the plan names them, with GN targets named after the directories and the include prefix and namespace `fuchsia_process` and `fuchsia_kernel`, as Fuchsia's own C++ bindings spell them.

**Reference**
- Fuchsia `ProcessLauncherState` and `ProcessLauncher::serve()`: the state, the decode error that drops the channel, and the reset after both Launch arms (`src/sys/lib/elf_runner/src/process_launcher.rs:57-63`, `:87-150`); `create_process_builder()`, which refuses a NUL in the name or an argument (`:176-238`); `LauncherError::as_zx_status()` (`:35-43`).
- `ProcessBuilder::add_handles()`: the reserved types, NsDir, and checking before adding (`src/lib/process_builder/src/process_builder.rs:308-360`); `add_namespace_entries()`, which numbers NsDir arguments in order (`:377-412`).
- `ProcessargsError::as_zx_status()` and `Message::build_header()`, TooManyHandles and SizeTooLarge as INVALID_ARGS (`src/lib/process_builder/src/process_args.rs:24-28`, `:189-230`).
- `HandleInfo::try_from(u32)` (`src/lib/fuchsia-runtime/src/lib.rs:263-281`).
- `sdk/fidl/fuchsia.process/launcher.fidl:16-185` (HandleInfo, NameInfo, LaunchInfo, Launcher); `sdk/fidl/fuchsia.kernel/debug-resource.fidl:11-16`.
- fdio_spawn's order of calls: AddArgs, AddHandles, AddNames, Launch (`sdk/lib/fdio/spawn.cc:897`, `:638`, `:655`, `:989`).
- Component-framework design D13, D19, D20, "The protocol subset" and the Launcher connection under "States, ownership and locking".

**Verification**
- `host/obj/system/lib/fuchsia-process/test/fuchsia_process`: `[  PASSED  ] 21 tests.`
- `host/obj/system/lib/fuchsia-kernel/test/fuchsia_kernel`: `[  PASSED  ] 5 tests.`
- `host/obj/system/lib/launch/test/process_launcher`: `[  PASSED  ] 14 tests.`, among them 62 handles accepted and 63 answered InvArgs (in one message, in two, and as a 63rd namespace entry), 4052 bytes of strings accepted with the processargs at exactly 4096 bytes and one byte more answered InvArgs, ten handles with strings that fit only without them answered InvArgs, each reserved type refused, and the reset after an Ok and after a failed Launch.
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests, `OK`; the five new ordinals are recomputed from their selectors, and one digit changed in DEBUG_RESOURCE_GET's makes it fail and name the right value.
- Mutation checks, each made by hand and reverted: removing the mark check, the NsDir refusal, the reset after Launch, the OutOfRange mapping, the hand-over of handles to the hook, the closing of a malformed message's handles, or the NsDir numbering fails at least one state test; removing the decoders' trailing-bytes, size, pad, handle-count, name-length or reply-shape checks fails at least one codec test.
- The three suites also pass rebuilt by hand with `-fsanitize=undefined,bounds -fsanitize-trap=all` and `_LIBCPP_HARDENING_MODE_DEBUG`. The arena's size check cannot be seen without them, since Launch's own size check gives the same answer; under them, removing it traps, because the copy into the arena goes through a checked `std::span::subspan()`.
- The three new sources pass `-fsyntax-only` with the user toolchain's flags (`x86_64-unknown-none-elf`), where step 14 will build them; no user program links them yet.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed, 0 error lines), rel PASS (22 passed, 3 skipped, 0 failed, 0 error lines), the same as before the change.
- clang-format clean; clang-tidy reports nothing on the six new C++ files or the three headers.

**Deliberately not done**
- The Launcher server itself, the slot it runs in and the real start hook (channel pair, `launch::start()`, channel_write): step 14.
- Refusing unknown handle types, as Fuchsia's `HandleInfo::try_from()` does: the check would need a list of every `ours::HandleType`, kept in step with the enum by hand, and ort's decoder already ignores a type it does not know.
- Refusing a VmoVdso from the client: Fuchsia does not refuse it either, and treats one with argument 0 as a replacement vDSO (`process_builder.rs:348-355`); ours has one vDSO, so whether the Launcher should refuse it, as it refuses VmaRoot, is a question for the user, recorded in the step's report.
- AddEnvirons, SetOptions and CreateWithoutStarting: design non-goals; their ordinals close the connection like any other unknown one.
- Checking that a namespace path is absolute: fuchsia.io's Path invariants are not enforced by FIDL, and fsio refuses a relative path when it binds the entry (step 5).

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- AddHandles is one-way. When a client sends a 63rd handle, how does it find out, and why not by having the connection closed?
- Why may a client not send its own VmaRoot or NsDir through AddHandles, and what could a child do with each if the Launcher let it through?
- A client adds ten handles and 4040 bytes of arguments. The state takes both in, yet Launch answers InvArgs. Which two numbers did Launch add up that the Add* calls could not?
