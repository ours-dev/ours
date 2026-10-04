# vfs changes

## Title: system/lib/vfs serves the fuchsia.io subset over a tree of pseudo-directories, VMO files and services, one message at a time and without touching a channel (2026-10-04)

**Files**
- `system/lib/vfs/` — new library, scaffolded with copier from `tools/templates/cpp_library` (the generated `vfs.hpp` and `vfs.cpp` placeholders replaced by the two headers below)
- `system/lib/vfs/include/vfs/tree.hpp`, `src/tree.cpp` — `vfs::Tree` and `vfs::Node`: the three node kinds (`NodeKind::Directory`, `File`, `Service`), `VmoFile`, children kept in name order, `lookup()` walking a path one segment at a time, and `is_valid_name()` / `is_valid_path()`, fuchsia.io's Name and Path invariants
- `system/lib/vfs/include/vfs/server.hpp`, `src/server.cpp` — `vfs::Server` and `vfs::Connection`: the connection table with generation-checked `ConnectionId`s, `bind()`, `dispatch()` returning an `Outcome`, `release()`, the Open checks, and the handlers of ReadDirents, Read, GetBackingMemory, Clone and Close; `Syscalls`, the one system call the server is given
- `system/lib/vfs/test/tree_test.cpp` — 9 host tests: names, paths, name order, refusals, lookup
- `system/lib/vfs/test/server_test.cpp` — 32 host tests: the Open check order, rights shrinking, the EXECUTE gate, ReadDirents, Read, GetBackingMemory, Clone, Close, a full table, services, malformed messages and `bind()`
- `system/lib/vfs/BUILD.gn`, `test/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — the library's targets and the copier template's files
- `BUILD.gn` — `//system/lib/vfs:tests` joins the default build on the host toolchain
- `docs/repairs/261004/vfs.md` — two performance items, the linear child lookup and the linear connection-slot scan

**Symptom**
None; this is step 4 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Nothing serves a file protocol yet. BootServer (/boot) and each component's /svc in the root (steps 13 and 14) are built on this library.

**Cause**
A file server is mostly a set of refusals. The root will serve /boot and /svc to programs it does not trust, and every rule below exists because leaving it out gives a client something it should not have. The rules are the Rust vfs's (`src/storage/lib/vfs/rust`), checked in its order, with the departures design D21 chose and a few more listed under Change.

- **Why the core touches no channel.** `Server::dispatch()` takes one message as it came off a channel and returns an `Outcome`: write this here, wait on that handle, hand this channel to that service, or drop the connection. The program does the writing, closing and waiting. There are three reasons. First, the whole protocol, the Open checks included, can then be tested on the host, message by message, with handles as plain numbers; on the target it could only be tested through a booted root. Second, the root runs everything on one thread (D16), so a vfs that wrote on channels itself would have to know the Loop's rules: a write is fire-and-forget (I1), and only a slot's own callback may close its handle (I3). With an `Outcome`, the Loop's code is the only code that touches handles, and the vfs cannot break either rule. Third, it makes every effect visible at one place in the caller. This is the "functional core, imperative shell" split; the old launcher's protocol (`launch/protocol.hpp`) and the codecs of step 3 are pure in the same way. The one system call the core cannot avoid is the handle duplicate GetBackingMemory hands out, so the program passes it in as a function pointer (`Syscalls`), and a test passes a recorder.
- **Why connections are named by an index and a generation.** The table is a fixed array, because user programs have no heap (`system/lib/ort/src/cpp_dependencies.cpp`). A slot is reused once its connection ends. If an id were only the index, a Loop packet still queued for the old connection would be delivered to the new one in the same slot: a different client, with different rights. So `ConnectionId` is `{index, generation}`, the generation goes up on every release, and an id from before the release names nothing; `dispatch()` answers it `Drop`. The Loop keys its packets the same way (FS §4.2).
- **Why the order of the Open checks matters.** Each check alone is easy; the order decides which error a request that is wrong in several ways gets, and so what a client can learn about a tree it may not see. The order is the Rust vfs's `handle_open`: path syntax (BadPath), then TRAVERSE for anything but "." (AccessDenied), then the requested rights against the connection's (AccessDenied), then CONNECT for PROTOCOL_SERVICE (AccessDenied); only after these is the tree looked at. So a connection without TRAVERSE gets AccessDenied for `bin` and for `nope` alike: it cannot probe which names exist. Had the lookup come first, NotFound against AccessDenied would leak exactly that.
- **Why a connection never grows.** Check 3 refuses any right the parent connection does not hold. A namespace entry is a connection the root binds with the route's rights (rx* for /boot), so everything opened through it is bounded by those rights, however deep. This is the whole access-control model of a namespace: there is no ambient root and no absolute path on the server side, so the rights on the handle a process was given are its ceiling.
- **Why rights are shrunk on files but refused when they cannot be honoured.** `PERM_READABLE` asks for CONNECT, ENUMERATE, TRAVERSE, READ_BYTES and GET_ATTRIBUTES. A file has no use for the first three, and a client opening a file with the same flags it uses for directories is normal, so the Rust vfs keeps only the rights the file protocol uses (`ALLOWED_RIGHTS`) and does not complain (D21). WRITE_BYTES and EXECUTE are different: the file protocol uses them, so asking for them is asking for something real, and a file that cannot honour them refuses the Open with AccessDenied (`new_connection_validate_options`). Directories keep every right asked for, because READ_BYTES and EXECUTE on a directory are the ceilings of the files under it.
- **Why the EXECUTE gate is where Execute is decided.** A VmoFile is added as executable or not; the root marks only BootFS files under `bin/` executable, and gives those VMOs the Execute right with `vmo_replace_as_executable` at ingest (D15). A file connection holds EXECUTE only if the Open passed this gate and the parent connection held EXECUTE; GetBackingMemory hands out a VMO with Execute only to a connection holding EXECUTE. So a VMO with Execute leaves the root only for a file the root chose and through a route that granted x. Invariant I6, that every executable VMO a client can hold has bytes nobody can write, rests on this, and D14 (b), parsing a client's VMO in place, rests on I6.
- **Why GetBackingMemory's PRIVATE_CLONE is a duplicate.** In Fuchsia a private clone is a copy-on-write snapshot, so the client keeps the bytes it saw even if the file later changes. ours's snapshot drops Execute (`syscalls.gni:851-852`), so fdio_spawn's `READ|EXECUTE|PRIVATE_CLONE` would come back without the right it needs. A BootFS file never changes, so the VMO itself is already a private copy in every sense that matters (D21). The rights handed out are `Basic|Map|GetProperty` plus Read and Execute as asked, as VmoFile does; Basic holds Duplicate, which `launch::start()` needs. `VmoFile` documents the precondition: nobody may be able to write the VMO.
- **Why ReadDirents lists "." first and answers BufferTooSmall.** The Rust vfs lists "." before a directory's children, then the children in name order (a BTreeMap), resuming from a cursor kept in the connection. When the first entry of a reply does not fit, the answer is BufferTooSmall and the cursor stays; an Ok with no entries would mean "the end", and the client would stop listing a directory that still has entries. When a later entry does not fit, the reply ends there and that entry is first next time. The end is an Ok with no bytes.
- **Why limits are OutOfRange.** A Read reply carries at most `MAX_TRANSFER_SIZE` (4072) bytes, and the Rust vfs answers a larger count with OutOfRange rather than a short read. ReadDirents' `max_bytes` has the same ceiling (`MAX_BUF`); the Rust vfs does not check it, relying on the FIDL vector bound, and the C++ vfs refuses it with BAD_HANDLE. ours answers OutOfRange for both, one rule for one limit.
- **Why malformed messages drop the connection without a word.** A message that fails to decode, comes with the wrong direction byte (I5), or names a method this connection does not serve is answered `Drop`: the program closes the channel and every handle the message carried, without an epitaph. Fuchsia's bindings do the same when a message fails to decode (`process_launcher.rs:92`, design "The protocol subset"). For an unknown method, a FIDL server decides by the dynamic flag the caller sets in the header when it calls a flexible method: a flexible call to an open protocol gets a reply saying the method is unknown, and a strict one closes the channel. ours's header has no such flag (byte 4 carries the direction instead), so the server cannot tell, and takes every unknown call as strict. That is the safe reading: ours's framing has no "unknown method" reply to answer a flexible call with, and a client given no answer would wait for ever, where a closed channel shows at once. The usual unknown call is a File method sent to a directory or the reverse, and those are strict in fuchsia.io (`file.fidl:192`, `:301`; `directory.fidl:476`), so the result is Fuchsia's. The one exception is Open sent to a file: Open is flexible (`openable.fidl:19`), so a Fuchsia file server ignores it, while this one drops the connection. Closing every carried handle matters: a client could otherwise park channel ends in the root's table with nothing that knows to close them.
- **Why a service hands its channel back.** In the Rust vfs a `Service` node holds a closure, and opening it calls the closure with the channel. Here the node holds a value the program chose, and `dispatch()` returns `Connect{handle, service}`. The program's code that runs next is the closure: in the root it routes the component's use (D9, D11) and forwards the channel to the provider or binds it to a built-in. That code writes on channels, which the core must not do.

**Change**
The tree, with storage the program owns:

```cpp
auto nodes = std::array<vfs::Node, 512>{};
auto tree = vfs::Tree(nodes);
auto const root = tree.add_root().unwrap();
auto const bin = tree.add_directory(root, "bin").unwrap();
auto const x = tree.add_file(bin, "x", {.vmo = slice, .bytes = mapped, .executable = true});
```

The server, and one message:

```cpp
auto server = vfs::Server(tree, connections, {.handle_duplicate = ours::handle_duplicate});
auto const boot = server.bind(server_end, root, fuchsia_io::RX_STAR_DIR).unwrap();
// a Loop callback, for a message read from |boot|'s channel:
auto const outcome = server.dispatch(boot, message, reply_buffer);
switch (outcome.action) { /* Reply, ReplyAndClose, Drop, Bind, Connect, Refuse */ }
```

Open, in `Server::open()`, checks in this order: (1) path syntax, BadPath; (2) TRAVERSE unless ".", AccessDenied; (3) requested rights a subset of the connection's, AccessDenied; (4) CONNECT for PROTOCOL_SERVICE, AccessDenied; (5) GET_ATTRIBUTES, unreachable since Open's options table is cut; (6) creation flags, AccessDenied without MODIFY_DIRECTORY, else NotSupported; (7) what the design cut, NotSupported; (8) the lookup, NotFound or NotDir; (9) the node's own rules: the protocol asked for (NotFile, NotDir, BadType), file-only flags (InvArgs), then for a file the rights shrunk to the file protocol's and the write and EXECUTE gates, for a service no other flag (InvArgs) and CONNECT.

Where this departs from the Rust vfs, and why (all within design D21 and the non-goals; none changes a design decision):
- Paths follow fuchsia.io's documented Path invariants and answer BadPath, as `openable.fidl:16` documents and D21 chose. The Rust vfs accepts a leading and a trailing '/' and answers INVALID_ARGS for an invalid segment (`src/storage/lib/vfs/rust/src/path.rs:47-60`; `src/storage/lib/vfs/rust/name/src/lib.rs:169-174`).
- Creation is refused: AccessDenied without MODIFY_DIRECTORY, as the Rust vfs checks first, and NotSupported with it, before the lookup. The Rust vfs would open an existing node for FLAG_MAYBE_CREATE and answer ALREADY_EXISTS for FLAG_MUST_CREATE. A client creates nothing here (non-goal: mutations).
- PROTOCOL_NODE and FLAG_SEND_REPRESENTATION answer NotSupported: node-only connections and OnRepresentation are cut, and serving such a request without them would leave the client waiting for an event or speaking the wrong protocol.
- PERM_INHERIT_WRITE and PERM_INHERIT_EXECUTE are ignored, as the design cuts them: the connection gets exactly the rights it names. The Rust vfs would add the parent's write or execute rights to a directory connection.
- Opening a service needs CONNECT however the request is spelled. The Rust vfs checks CONNECT only when the flags name PROTOCOL_SERVICE (check 4 above), and `to_service_options` accepts empty flags, so a request with no flags reaches a service from a connection without CONNECT. No connection in ours lacks CONNECT today (every rights word includes it), so this costs nothing and closes the gap before something depends on it.
- ReadDirents above `MAX_BUF` answers OutOfRange (see Cause).
- A service node returns `Connect` to the program instead of calling a closure (see Cause). The design says "the node's callback routes the component's use"; that callback is the program's handling of `Connect`.
- `bind()` serves directories only: every namespace entry and /svc is one.

Rejected:
- A `Service` node holding a function pointer and context that `dispatch()` calls. It would put a channel write inside the core, out of the host tests' reach and out of the Loop's control.
- A virtual `Node` base with three subclasses, as the Rust vfs's traits are: the three kinds are fixed at build time, and a base class needs the user's approval; a `NodeKind` and one storage type cost one switch.
- Templates on the pool sizes (`Tree<N>`, `Server<N>`): spans let the program size and place the storage, and one copy of the code serves every size.
- Leaving the channel out of the connection record. The design's vfs connection holds it, and a Loop callback finds its channel from its connection; the record never writes or closes it, so the Loop slot stays its only owner.

**Reference**
- Fuchsia Rust vfs at `51f05104c13`, in `src/storage/lib/vfs/rust/src/`: `DirectoryConnection::handle_open` (`directory/connection.rs:385-417`), its INHERIT stripping (`:299-305`) and `handle_read_dirents` (`:471-490`); `Simple::open_impl` and `Simple::read_dirents` (`directory/simple.rs:139`, `:282`); `read_dirents::Sink` and its BUFFER_TOO_SMALL rule (`directory/read_dirents.rs:48-72`); `ProtocolsExt for fio::Flags`: `is_dir_allowed` and the others (`protocols.rs:229-242`), `to_directory_options` (`:267-291`), `to_service_options` (`:306-329`), `to_file_options` and `ALLOWED_RIGHTS` (`:414-446`, `:431-436`), `flags_to_rights` (`:529-531`); `new_connection_validate_options` and `get_backing_memory_validate_flags` (`file/common.rs:15-36`, `:61-90`); `FileConnection::handle_read` (`file/connection.rs:859-868`) and `handle_clone` (`:848-857`); `VmoFile::get_backing_memory` and `get_as_private` (`file/vmo.rs:180-193`, `:212-224`); `Service::connect` and `service::serve` (`service.rs:89-121`, `:154-166`); `Path::validate_and_split` (`path.rs:35-71`).
- Fuchsia C++ vfs: `DirectoryConnection::ReadDirents` refusing `max_bytes > kMaxBuf` (`src/storage/lib/vfs/cpp/connection/directory_connection.cc:399-417`); `StreamFileConnection::ReadInternal` (`connection/stream_file_connection.cc:38-50`).
- fuchsia.io: Name and Path (`sdk/fidl/fuchsia.io/io.fidl:9-47`), Open's BAD_PATH (`openable.fidl:16`), Operations and the rights words (`rights-abilities.fidl`).
- `docs/design/261004/component-framework.md`: the vfs object, "Events: a component opening a file in /boot", I1, I3, I5, I6, D14, D15, D16, D17, D21, D27; `docs/design/260924/userspace-filesystem.md` §2.2-§2.6 and §4.2, as revised by D21.

**Verification**
- `out/bringup.x86/host/obj/system/lib/vfs/test/vfs`: `[  PASSED  ] 41 tests.` (9 tree, 32 server).
- Mutation check, each change made alone and the tests rerun: removing the TRAVERSE check fails 2 tests, the rights-subset check 2, the CONNECT check for PROTOCOL_SERVICE 1, the EXECUTE gate 1, file rights shrinking 1, the cut-flags check 1, the CONNECT check at a service 1, the Read limit 1, Execute on the duplicate 1, GetBackingMemory's EXECUTE check 1, ReadDirents' BufferTooSmall 1, name order 1, the seek update 2; moving the path check after the rights checks fails 1; removing ReadDirents' limit traps (an out-of-range `span::first`).
- The suite also passes rebuilt by hand with `-fsanitize=undefined,bounds -fsanitize-trap=all` and `_LIBCPP_HARDENING_MODE_DEBUG`, with fuchsia-io and wire rebuilt the same way.
- Both sources pass `-fsyntax-only` with the user toolchain's flags (`--target=x86_64-unknown-none-elf`), where step 13 will build them; no user program links the library yet.
- clang-format clean; clang-tidy reports nothing on the two sources, the two tests and, with `--header-filter`, the two headers.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed, 0 error lines), rel PASS (22 passed, 3 skipped, 0 failed, 0 error lines), the same as before the change: no image links the library yet.

**Deliberately not done**
- No Loop and no channel I/O: the program's half (read a message, act on the `Outcome`) is the root's, in step 13, on the Loop of step 12.
- No BootServer: building the tree from the BootFS at ingest, slices made executable under `bin/`, is step 13's.
- No Rewind, Seek, GetAttributes, ReadAt, OnRepresentation, Watch, writes or symlinks: design non-goals. Their requests are dropped as unknown methods.
- No per-client limits on connections (D18): a full table answers NoResource to whoever asks next.
- No inode numbers: every entry, "." included, reports `INO_UNKNOWN`, as the Rust vfs's pseudo nodes do by default.
- The linear child lookup and the linear slot scan are left as they are and recorded in `docs/repairs/261004/vfs.md`: performance work goes through the user, item by item.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A component holds /boot bound with TRAVERSE taken away. It opens `bin` and then `nope`. What does each answer, and what would it learn if the lookup came before the TRAVERSE check?
- A client opens `bin/x` with `PERM_READABLE | PERM_EXECUTE` through a /boot connection bound rx*. Which rights does the new connection hold, which of the requested rights were dropped and which would have been refused, and what decides the difference?
- Why does GetBackingMemory with PRIVATE_CLONE get the same duplicate here when Fuchsia makes a snapshot, and which property of the BootFS makes that safe?

## Title: a vfs ConnectionId with generation 0 never names a live connection (2026-10-04)

**Files**
- `system/lib/vfs/include/vfs/server.hpp` — `FIRST_GENERATION` (1), every slot's starting generation, and the rule on `ConnectionId`
- `system/lib/vfs/src/server.cpp` — `Server::release()` skips 0 when a slot's generation wraps
- `system/lib/vfs/test/server_test.cpp` — `GenerationZeroNamesNoConnection`

**Symptom**
Latent, cannot be triggered today: nothing kept a ConnectionId it had not been given. With step 13 the root keeps one per Loop slot (`BootServer::Storage::bindings`), in a table that starts zeroed; any entry read before it was filled in would have been `{0, 0}`, and that named the first connection ever made in slot 0, which was live. A dispatch through it would have served another client's connection, and a release through it would have ended that connection under its client.

**Cause**
A ConnectionId is a slot index plus the generation the slot was in when the connection was made, so that an id kept past its connection's end goes stale instead of naming whatever comes next in that slot. That only works if no id that nobody issued can match a live slot. Every slot started at generation 0, and 0 is also what a zeroed or default-constructed id holds: `ConnectionId{}` was, by accident, a valid id for slot 0's first connection. The generation scheme protected against an id outliving its connection, but not against an id that never belonged to one. Zircon closes the same hole for handle values: a free slot's first handle gets generation 1 (`NewHandleValue()` adds 1 to a generation of 0), and `map_handle_to_value()` sets the low bits, so no valid handle value is 0, `ZX_HANDLE_INVALID`.

**Change**
Before, a slot started at 0 and release added one, wrapping to 0:

```cpp
u32 m_generation = 0;
...
slot->m_generation = id.generation + 1;
```

After, it starts at 1 and skips 0 when it wraps:

```cpp
inline constexpr auto FIRST_GENERATION = u32{1};
u32 m_generation = FIRST_GENERATION;
...
auto next = id.generation + 1;
if (next == 0) {
    next = FIRST_GENERATION;
}
```

So `{*, 0}` names no live connection, ever. This was the recommended option of the open review point on step 4.

Rejected: keeping 0 valid and making every holder of an id carry a "filled in" flag beside it. That moves the rule to every caller, where the type can carry it once.

**Reference**
Zircon `NewHandleValue()` (`zircon/kernel/object/handle.cc`, the generation starting at 1 for a slot's first handle) and `map_handle_to_value()` (`zircon/kernel/object/handle_table.cc:18`, `kHandleMustBeOneMask`, so that no handle value is `ZX_HANDLE_INVALID`).

**Verification**
- `out/bringup.x86/host/obj/system/lib/vfs/test/vfs`: `[  PASSED  ] 42 tests.`, the new test among them: slot 0's first connection has generation 1, and `ConnectionId{}` is dropped while it is live, leaving it alone.
- `boot_check.py`: dbg PASS, rel PASS, as recorded in `init.md`'s step 13 entry, where the root first serves connections.

**Deliberately not done**
- The Loop's keys (`loop::Key`) start at generation 0 too, and the same reasoning applies to a key nobody issued. Nothing builds one by hand, and its packets come only from waits the Loop armed, so it is left as it is.

**How it was found**
Review of step 4, before its first caller existed.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the generation check enough to make a released connection's id stale, but not enough to make a zeroed id stale, before this change?
- Zircon reaches the same property for handle values in two ways at once. What are they, and which one does this change copy?
- After 2^32 releases of one slot, what generation does the next connection get, and why not 0?
