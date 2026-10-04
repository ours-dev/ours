# fsio changes

## Title: system/lib/fsio holds a process's namespace, cleans paths as fdio does, and makes the fuchsia.io calls through them (2026-10-04)

**Files**
- `system/lib/fsio/` — new library, scaffolded with copier from `tools/templates/cpp_library`
- `system/lib/fsio/include/fsio/path.hpp`, `src/path.cpp` — `PathBuffer` and `clean_path()`, fdio's `CleanPath` on a `string_view`, refusing a path longer than `MAX_PATH_LENGTH` (1024)
- `system/lib/fsio/include/fsio/namespace.hpp`, `src/namespace.cpp` — `Namespace`, a flat table of up to 16 bound paths (1024 bytes of path text in all), with `bind()`, `unbind()` and `resolve()` answering what fdio's `fdio_ns_bind()`, `fdio_ns_unbind()` and `fdio_ns_open3()` answer
- `system/lib/fsio/include/fsio/fsio.hpp`, `src/fsio.cpp` — user toolchain only: `installed_namespace()`, `install()` from the NsDir startup handles, `open()` (two overloads), `connect()`, `read()`, `read_dirents()`, `get_backing_memory()` and `close()`
- `system/lib/fsio/BUILD.gn` — `:namespace` builds everywhere, the calls (`//system/lib/fsio`) only in the user toolchain, and `:tests`
- `system/lib/fsio/test/path_test.cpp`, `namespace_test.cpp` — 30 host tests: fdio's own CleanPath cases and the length limit; every bind, unbind and lookup rule
- `system/lib/fsio/README.md` — what the library holds and how it is built
- `BUILD.gn` — `//system/lib/fsio:tests` on the host, and `//system/lib/fsio` in the user toolchain, so that the calls compile before anything links them
- `docs/repairs/261004/fsio.md` — the read path's fourth copy and fixed per-call costs, to measure before anything is changed

**Symptom**
None; this is step 5 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). No process in the tree has a namespace yet. The first user is `routing_checks`, which the root starts in step 13; the shell follows in step 15. Export and spawn wait for step 14, since spawn speaks the Launcher codec of step 6.

**Cause**
Why a process needs this library, and why each of its rules is there.

- **Why the namespace lives in the client.** Linux has one table of mounts (per mount namespace), kept in the kernel, and every path a process names is walked through it by the kernel's own `path_lookupat()`. Fuchsia has no such table. Each process gets a handful of directory channels at start, each tagged with the path it should appear at (`PA_NS_DIR` handles, whose names travel in the processargs message). The process keeps the table from path prefix to channel itself, in fdio. Opening `/boot/bin/x` means: find the entry `/boot`, and send `Open("bin/x")` on its channel. The server never sees the absolute path, and has no idea where in the client's namespace it is mounted. A process can reach only what it was handed: the namespace is a list of capabilities, not a view of one global tree. This is the model the user chose in S1 (design: "a namespace per process, as fdio_ns"). fsio is the table and the code around it, nothing more. The kernel knows nothing about paths.
- **Why the client cleans the path before it looks anything up.** The lookup is a string comparison against the bound prefixes. Take `/boot/../svc/fuchsia.process.Launcher`, uncleaned. It starts with `/boot/`, so it would go to the /boot server as `Open("../svc/fuchsia.process.Launcher")`. A fuchsia.io server must refuse that: a valid path has no "." or ".." names (`io.fidl`, the Path alias), and a directory connection has no parent the server could climb to anyway. The connection *is* the capability, so letting ".." walk above it would hand out something the client was never given. Cleaned first, the path becomes `/svc/fuchsia.process.Launcher`, which is what the caller meant, and goes to the /svc server. Cleaning is therefore not cosmetic: it decides *which server* gets the request, and it is the only reason a client may write ".." at all.
- **Why ".." is lexical.** Cleaning drops a ".." together with the name before it in the *text* (rule 3), without asking any server what the parent is. This is Plan 9's answer in "Lexical File Names in Plan 9 or Getting Dot-Dot Right", which fdio's `CleanPath` follows through Go's `filepath.Clean`. Linux instead resolves ".." physically in the kernel's path walk (`follow_dotdot()` in `fs/namei.c`): if `/a/link` is a symbolic link to `/x/y`, then `/a/link/..` is `/x` in Linux and `/a` lexically. ours has no symbolic links, so the two readings cannot differ yet. When links arrive, the lexical reading stays, as the FS design decided (`docs/design/260924/userspace-filesystem.md` §5).
- **Why rules 4 and 5 exist.** A rooted path has nothing above "/", so "/.." is "/" (rule 4); without that rule, `/boot/../../svc` would clean to something above the root. A relative path's start is unknown, so a leading ".." cannot be cancelled against anything and is kept (rule 5). The `dotdot` mark in the code is the length below which the output can no longer shrink: the root, or the run of leading ".." names. `"../a/../.."` cleans to `"../.."`, not to `".."`.
- **Why the length check is on the input, and why it is enough.** fdio refuses a path whose *input* reaches `PATH_MAX`. fsio does the same with `MAX_PATH_LENGTH`, 1024, the longest path Open carries in a 4 KiB message. The output buffer is never overrun because cleaning never writes more than it consumes. A name copies itself. The '/' written before a name stands for the separator before it in the input, which step 1 consumed without writing. A kept "/.." stands for the same three input bytes. The only exception is the empty path, which becomes "." (one byte). The test `NeverLonger` checks the two worst cases, 341 ".." names and 512 one-letter names, which keep every byte.
- **Why bound paths may not overlap, and why that makes a flat table enough.** fdio keeps the namespace as a tree of nodes. Each mount point is a "remote" leaf, and every node above a mount point is an "intermediate" node fdio made to hold it. Two rules follow, and fdio enforces both in `fdio_namespace::Bind()` (`local-filesystem.cc:472`):
  - nothing may be bound below a mount point. Binding `/boot/bin` while `/boot` is bound would hide part of a directory that another server serves; the walk "implies shadowing, which is not supported" (NotSupported);
  - nothing may be bound at a node that already holds mount points below it. Binding `/data` while `/data/cache` is bound finds the node existing (AlreadyExists), and binding "/" over a non-empty tree is an "overlay", which is "disallowed" (NotSupported).

  So no bound path is a prefix of another, and any path lies under at most one of them. fdio's walk finds the longest bound prefix; with no overlap, that is the *only* bound prefix, so a linear scan of a flat table returns the same answer. The flat table needs no node pool and no heap, which a user program does not have.
- **Why prefixes are compared at a name boundary.** `/bootfs/x` starts with the characters `/boot` but does not lie under `/boot`. `lies_below()` therefore requires the next character to be '/', or the outer path to be "/" itself. Remove that check and the tests show the classic bug: `/bootfs` resolves to the /boot server, and `/1` is taken for a prefix of `/10`.
- **Why "/" is NotSupported when nothing is bound there.** In fdio the root is always a node, an intermediate one until "/" is bound. `OpenRemote("/")` walks to that node and refuses with NotSupported ("Cannot connect to non-mount-points", `local-filesystem.cc:262`), even in an empty namespace. NotFound would claim the root does not exist. The same answer comes back for every node above a mount point, such as `/data` when only `/data/cache` is bound.
- **Why the table never closes a handle.** `Namespace` makes no system call, so it builds on the host and every rule above is a host test. The price is ownership written into the interface. On a successful `bind()` the table holds the channel. On any failure the caller still holds it. `unbind()` hands the channel back for the caller to close, where `fdio_ns_unbind()` closes it itself. The user-only half restores fdio's behaviour where it matters: `install()` closes a handle it could not bind, as `fdio_ns_bind()` does.
- **Why `install()` takes the handles.** fdio binds each `PA_NS_DIR` handle but leaves it in its startup array, so that "higher level code that may also need access to the namespace" can see it too (`unistd.cc:628-636`). ours's runtime hands out each startup handle once (`ort::take_startup_handle()`), so that every handle has exactly one owner. A handle that two parts of a program both believe they own gets closed twice, and the second close may hit a new handle that reused the value. fsio takes the NsDir handles, and the namespace is their one owner.
- **Why `open()` consumes `object` on every path.** `channel_write` consumes the handles it is given, whether it succeeds or fails. If `open()` returned early, after cleaning or resolving failed, without closing `object`, the caller would have to know which failures left it the owner. One rule is simpler and cannot leak: the server end is gone when `open()` returns. fdio gets the same by wrapping it in a `zx::channel` at entry (`fdio_open3()`, `directory.cc:146-153`).
- **Why a failed Open shows up later, and how it is told apart.** Open is one-way, so `open()` cannot learn whether it worked. The server reports a failure as an epitaph on `object` and closes it. The caller's first two-way call on its end then reads the epitaph instead of a reply. `not_a_reply()` turns that into the epitaph's status, so `read()` after a failed `open("/boot/missing")` answers NotFound, not "malformed". An Ok epitaph means the server closed with nothing wrong, but the request was still never answered, so it is PeerClosed.
- **Why each call carries a fresh txid.** A process has one thread and one call in flight, so a txid of 1 would always match. But if a call wrote its request and then gave up before reading the reply, that reply would stay queued. The next call on the same channel would read it as its own answer. With a counter, the stale reply's txid does not match, and the decoder refuses it.
- **Why `Incoming` closes the handles nobody claimed.** A server, or anything holding the other end, can attach up to 64 handles to any message, a malformed one included. The decoders refuse a reply whose handle count is wrong, but refusing does not close anything. Without the destructor, a hostile server could fill the client's handle table one Read at a time. `get_backing_memory()` is the only call that keeps a handle, and it claims it with `take_handle()` only after the reply decoded as Ok.
- **Why `read()` refuses more bytes than it asked for.** The reply's size field comes from the server. Copying `size` bytes into a buffer of `count` would let the server write past the caller's buffer. zxio makes the same check and answers `ZX_ERR_IO` (`RemoteReadv`, `sdk/lib/zxio/remote.cc:54-57`). ours has no I/O status, and such a reply is not a well-formed answer to the request, so the answer is InvArgs, as for any malformed reply.
- **Why everything is `constinit` and fixed-size.** Nothing runs a user program's dynamic initialisers: there is no `.init_array` walk (`system/lib/ort/src/start.cpp`). So a global `Namespace` must be constant-initialised, or it would silently start as zeros, which here happens to be an empty table, but only by luck. `Namespace` and `PathBuffer` have constexpr constructors, and a test declares a `constinit Namespace` to keep it that way. There is no heap either, so the table has fixed bounds: 16 bindings, enough for a component's directory uses plus /svc, and 1024 bytes of path text shared by all of them.

**Change**
The three layers, from the bottom:

```cpp
// fsio/path.hpp: cleaned text, writable only by clean_path().
class PathBuffer final { ... std::array<char, MAX_PATH_LENGTH> m_chars; usize m_size; };
auto clean_path(std::string_view path, PathBuffer &out) -> ours::Status;

// fsio/namespace.hpp: no system calls, host-tested.
class Namespace final {
    auto bind(std::string_view path, ours::Uandle directory) -> ours::Status;
    auto unbind(std::string_view path) -> ustl::Result<ours::Uandle, ours::Status>;
    auto resolve(PathBuffer const &path) const -> ustl::Result<Resolved, ours::Status>;
};
struct Resolved { ours::Uandle directory; std::string_view rest; };   // rest is "." or "a/b"

// fsio/fsio.hpp: user programs only.
auto install() -> ours::Status;
auto open(std::string_view path, fuchsia_io::Flags flags, ours::Uandle object) -> ours::Status;
auto open(std::string_view path, fuchsia_io::Flags flags) -> ustl::Result<ours::Uandle, ours::Status>;
auto connect(std::string_view path, ours::Uandle server) -> ours::Status;
auto read(ours::Uandle file, std::span<u8> buffer) -> ustl::Result<usize, ours::Status>;
auto read_dirents(ours::Uandle directory, std::span<u8> buffer) -> ustl::Result<usize, ours::Status>;
auto get_backing_memory(ours::Uandle file, fuchsia_io::VmoFlags flags) -> ustl::Result<ours::Uandle, ours::Status>;
auto close(ours::Uandle node) -> ours::Status;
```

`resolve()` takes a `PathBuffer`, not a `string_view`, so the type says that the path was cleaned: only `clean_path()` writes one. A ".." can therefore never reach the lookup. With `/boot` and `/svc` bound:

```text
/boot                          -> boot  "."
//boot/./bin//x/               -> boot  "bin/x"
/boot/../svc/x                 -> svc   "x"
/bootfs                        -> NotFound
/                              -> NotSupported
boot/bin                       -> NotFound   (relative: no working directory)
```

The table keeps every path's text back to back in one 1024-byte array, each entry an offset and a size. `unbind()` closes the gap: it moves the later text down and lowers the later offsets. A test unbinds the middle one of three and checks the other two, and checks that the freed bytes can be bound again.

Statuses follow fdio entry for entry: `bind()` gives BadPath, BadHandle, InvArgs, BadPath for a long name, AlreadyExists, NotSupported, then NoResource where fdio would fail to allocate; `unbind()` gives BadPath, InvArgs, BadPath under a root binding, NotFound; `resolve()` gives NotFound, BadPath, NotSupported, NotFound. One difference is deliberate: fdio checks a name's length only where its walk reaches it, so a long name it never reaches is not reported. fsio checks every name first. A long name below a mount point is one the server would refuse with BadPath anyway.

The calls are fdio's and zxio's, one for one: `open()` is `fdio_open3()`, `connect()` is `fdio_service_connect()` (PROTOCOL_SERVICE alone), `read()` is one round of zxio's `RemoteReadv`, and `close()` is zxio's `Close()` followed by closing the handle. Each two-way call writes its request, waits for READABLE or PEER_CLOSED, reads into a full-size `Incoming` (4 KiB and 64 handles, so that a read can never fail with BufferTooSmall), and decodes with `//system/lib/fuchsia-io`.

Rejected:
- fdio's tree of nodes: it needs a node per path component and a heap; under the no-overlap rule, the flat table gives the same answers.
- A file-descriptor table (FS §5's `fd_table[256]`): no step needs POSIX, and a connection's client end already is the handle a caller holds.
- `CleanPath`'s `is_dir` output: only fdio's POSIX `open()` uses it, to add PROTOCOL_DIRECTORY for a trailing '/'. `fdio_open3()`, which `open()` mirrors, ignores it.
- A working directory: D29 settles that the shell has none, and a relative path answers NotFound, as in `fdio_ns_open3()`.
- `read()` looping until the buffer is full, as `zxio_chunked_do_vector` does: one request per call is enough for routing_checks and the shell's `ls`, and a short read is legal.

**Reference**
- fdio `CleanPath()` (`sdk/lib/fdio/cleanpath.cc:21`), and its tests, ported case for case (`sdk/lib/fdio/tests/fdio_cleanpath.cc`); Go `filepath.Clean`; Rob Pike, "Lexical File Names in Plan 9 or Getting Dot-Dot Right" (cited by fdio, not read locally).
- fdio's namespace: `fdio_namespace::WalkLocked()` (`sdk/lib/fdio/namespace/local-filesystem.cc:56`), `OpenRemote()` (`:241`, "Require that we start at /" at `:243`, "Cannot connect to non-mount-points" at `:262`), `Unbind()` (`:292`), `Bind()` (`:472`, "Overlay remotes are disallowed" at `:504`, "implies shadowing" at `:558`, `:589`), `Export()` (`:674`, for step 14).
- fdio's C entry points, each of which cleans first: `fdio_ns_open3()`, `fdio_ns_service_connect()`, `fdio_ns_bind()`, `fdio_ns_unbind()` (`sdk/lib/fdio/namespace/namespace.cc:52`, `:65`, `:118`, `:131`); `fdio_open3()` and `fdio_service_connect()` (`sdk/lib/fdio/directory.cc:146`, `:19`).
- fdio's start-up binding of `PA_NS_DIR` handles: `__libc_extensions_init()` (`sdk/lib/fdio/unistd.cc:586`, the case at `:628-636`).
- zxio: `RemoteReadv()` with its stack message buffer, its `actual > capacity` check and its copy out (`sdk/lib/zxio/remote.cc:37-61`); `Remote::Close()` (`:408`); `zxio_vmo_get_exec()` (`sdk/lib/zxio/get_vmo.cc:142-144`).
- fuchsia.io's Path invariants and the request that clients clean paths (`sdk/fidl/fuchsia.io/io.fidl:31-47`).
- Linux, by contrast: the kernel walks every path through the mount table (`path_lookupat()`, `follow_dotdot()` in `fs/namei.c`), and namespaces are copies of that one table (`copy_mnt_ns()` in `fs/namespace.c`), not per-process lists of capabilities.
- Design: component-framework "Events: a component opening a file in /boot" and the fsio object; FS §5 (`docs/design/260924/userspace-filesystem.md`), as revised by D29.

**Verification**
- `host/obj/system/lib/fsio/test/fsio`: `[  PASSED  ] 30 tests.` (11 CleanPath, 19 Namespace).
- The same tests rebuilt by hand with `-fsanitize=undefined,bounds -fsanitize-trap=all` and `_LIBCPP_HARDENING_MODE_DEBUG`: 30 passed.
- Mutation check, ten mutants, each caught: dropping the name-boundary test in `lies_below()` (fails PrefixesEndAtANameBoundary and BindStopsAtMaxBindings, where `/1` swallows `/10`), the shadowing refusal, the above-a-mount-point refusal, the offset fix-up in `unbind()`, `resolve()`'s name-length check, the root-node answer, rule 4, the `dotdot` mark, the input length limit, and an off-by-one in the capacity check (the run aborts).
- The calls compile in the user toolchain as part of the default build (`user-x86/obj/system/lib/fsio/src/fsio.fsio.cpp.o`); nothing links them yet, and they are first run on the target in step 13.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed, 0 error lines), rel PASS (22 passed, 3 skipped, 0 failed, 0 error lines), the same as before the change.
- clang-format clean; clang-tidy reports nothing on the three sources and three headers; on the tests, only googletest's own headers.

**Deliberately not done**
- Export by Clone and spawn: step 14, as the plan says; spawn speaks the Launcher codec of step 6.
- No target test of the calls: the plan checks them on the target from step 13, where routing_checks is the first program with a namespace.
- No fd table, no working directory, no POSIX layer, no `is_bound()`, no local (in-process) nodes: no step needs them.
- `BadPath`'s comment in `system/public/ours/status.hpp` still says "Nothing returns it yet", although fsio now does. That header is shared with steps running in parallel (the vfs of step 4 returns BadPath too), so the comment is left for whoever integrates the steps, rather than edited in two branches at once.
- The read path's copies and the zero-filled buffers are measured, not changed: `docs/repairs/261004/fsio.md`.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `/boot` and `/svc` are bound. Where does `/boot/../svc/x` go, and where would it go if fsio did not clean paths before looking them up? Why must the /boot server refuse what it would then receive?
- fdio refuses to bind `/boot/bin` when `/boot` is bound. What would a client lose if that were allowed, and why does the refusal let a flat table replace fdio's tree?
- `open("/boot/missing")` returns Ok. When, and through which message, does the caller learn that the file does not exist, and what status does `read()` then return?

## Title: fsio checks a reply's header for an epitaph first, and a namespace entry the root closed answers its epitaph's status from then on (2026-10-04)

**Files**
- `system/lib/wire/include/wire/wire.hpp` — `wire::is_epitaph(Header)`: the ordinal check a client makes before it decodes a reply
- `system/lib/wire/test/wire_test.cpp` — `WireEpitaph.IsEpitaphLooksAtTheOrdinal`
- `system/lib/fsio/include/fsio/namespace.hpp`, `src/namespace.cpp` — a binding's `closed` status; `Namespace::mark_closed()`; `resolve()` answers it for every path under a closed binding
- `system/lib/fsio/include/fsio/fsio.hpp`, `src/fsio.cpp` — every call asks `epitaph_of()` before its reply decoder; `open()` reads the epitaph of an entry whose server closed it and records its status with `mark_closed()`
- `system/lib/fsio/test/namespace_test.cpp` — three tests of the closed mark

**Symptom**
Latent until step 13: the root closes a namespace entry it cannot route with an epitaph before the component runs (design D8). Without this change, `fsio::open("/denied/x")` wrote Open on that closed channel and answered the write's PeerClosed, and the AccessDenied the root sent was never read: routing_checks could not tell "the root refused this entry" from "the root is gone". A second reason: the calls decoded a reply first and asked whether it was an epitaph only after the decoder refused it, so whether an epitaph was recognised depended on every reply decoder refusing the epitaph's bytes.

**Cause**
- **The epitaph on a namespace entry is a different case from the epitaph on a connection.** After a failed Open, the client's first call on its new connection reads the epitaph in place of a reply (step 5's `call()` handles that, including the write that meets a closed peer). An entry is different: the client never makes a call on the entry's own channel; it writes one-way Opens there and makes its calls on the new channels. So nothing ever reads the entry's epitaph unless `open()` does, at the moment its write finds the peer closed. And an epitaph is the channel's last message: once read, it is gone. If `open()` read it and did not keep it, the second `open()` would find an empty, closed channel and answer PeerClosed. That is why the status is cached in the binding (option (a) of the step 5 review): every later `open()` and `connect()` under the entry answers the same status, from `resolve()`, without touching the channel.
- **Why the header first.** A message that comes back on a call is the reply, or an epitaph sent instead of it. The two share the first 16 bytes' layout and differ in the ordinal: an epitaph's is `EPITAPH_ORDINAL`, which no method's can be, since method ordinals have their top bit clear. Asking the header first, `wire::is_epitaph()`, is the one test that cannot be fooled by a reply decoder that happens to accept 24 bytes; it also keeps "the server closed and said why" apart from "the server sent a malformed reply", which used to share a code path. Fuchsia's C++ client does the same: `ClientBase::Dispatch()` checks `maybe_epitaph()`, an ordinal comparison, before it matches a reply to its txid.

**Change**
Before, each call decoded the reply and fell back to the epitaph:

```cpp
auto const decoded = fuchsia_io::decode_read_reply(reply.message(), txid);
if (decoded.is_err()) {
    return ustl::err(not_a_reply(reply.message()));   // decode_epitaph() or InvArgs
}
```

After, the epitaph first, by its header:

```cpp
if (auto const closed = epitaph_of(reply.message()); closed.is_some()) {
    return ustl::err(*closed);                        // the status; Ok becomes PeerClosed
}
auto const decoded = fuchsia_io::decode_read_reply(reply.message(), txid);
if (decoded.is_err()) {
    return ustl::err(Status::InvArgs);
}
```

and `open()`, when its write finds the entry closed:

```cpp
if (written != Status::PeerClosed) {
    return written;
}
auto const closed = closed_status(resolved->directory);    // the epitaph's status, or PeerClosed
auto _ = installed.mark_closed(resolved->directory, closed);
return closed;
```

`Namespace::mark_closed()` keeps the first status (a binding closes once), refuses Ok, and leaves the channel in the binding, so `unbind()` still hands it back.

Rejected:
- Reading the entry's epitaph in `install()`, before anything is opened: a live entry has nothing to read, and peeking at it would need a non-blocking read on every entry at start for the one that failed.
- Unbinding a closed entry: the path would then answer NotFound, which loses the reason, and a later bind at the same path would be allowed where the component's manifest says the entry exists.

**Reference**
- Fuchsia `ClientBase::Dispatch()` (`sdk/lib/fidl/cpp/wire/client_base.cc:214-218`) over `IncomingHeaderAndMessage::maybe_epitaph()` (`sdk/lib/fidl/cpp/wire/incoming_message.cc:80-85`), and `kFidlOrdinalEpitaph`.
- Fuchsia closes a namespace entry it cannot route with `server.close_with_epitaph(e.as_zx_status())` in `NamespaceBuilder::serve()` (`src/sys/component_manager/lib/serve_processargs/src/namespace.rs:145-175`); fdio has no state for it, so a client there learns the reason only on its own connection.
- Design D8 and "Events: a component opening a file in /boot".

**Verification**
- `out/bringup.x86/host/obj/system/lib/fsio/test/fsio`: `[  PASSED  ] 33 tests.` (30 before, and three new: a closed binding answers its status at and under its mount point, beside others that stay open; `mark_closed()`'s refusals; the mark moving with its binding through `unbind()`).
- `out/bringup.x86/host/obj/system/lib/wire/test/wire`: `[  PASSED  ] 12 tests.`, the new one among them.
- On the target, routing_checks opens three paths under `/denied` and gets AccessDenied each time: the first from the epitaph, the second and third from the binding. With the root's offer widened to rx*, the same three opens answer Ok and the boot fails (`init.md`, step 13).

**Deliberately not done**
- A closed entry is not reopened or rebound: nothing in the design reroutes an entry after start (D8).
- `closed_status()` reads one message. The root writes nothing on an entry but the epitaph that closes it, so a reply left over there would only come from a call a program made on the entry's own channel and abandoned; PeerClosed is then the answer, which is still true.

**How it was found**
Review of step 5: the AccessDenied check routing_checks needed in step 13 had no way to read the root's answer.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A failed Open's epitaph is read by the first call on the new connection, but a failed namespace entry's epitaph needs `open()` to read it. What is different about the two channels?
- Why is the entry's status cached in the binding rather than read again each time, and what would the second `open()` under `/denied` answer without the cache?
- Why must a client ask whether a message is an epitaph before it decodes it as a reply, rather than after the reply decoder refuses it?
