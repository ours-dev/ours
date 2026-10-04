# fuchsia-io changes

## Title: system/lib/wire frames messages and system/lib/fuchsia-io encodes the fuchsia.io subset by hand (2026-10-04)

**Files**
- `system/lib/wire/` — new library, scaffolded with copier from `tools/templates/cpp_library`: the 16-byte `Header` with its `Direction` byte, `HANDLE_PRESENT`, `EPITAPH_ORDINAL`, the 24-byte status form, `Method` (selector, ordinal, kind), `Message`, and the checks every codec shares (`decode_header`, `decode_request_header`, `decode_reply_status`, `start_message`, `start_request`, `start_ok_reply`, `encode_status_reply`, `encode_epitaph`, `decode_epitaph`, `encode_handle`, `decode_handle`)
- `system/lib/wire/test/wire_test.cpp` — 11 host tests: the header, txid rules, the status form, the epitaph and handle fields, each broken byte by byte
- `system/lib/wire/test/ordinals_test.py` — reads every `wire::Method` declaration out of the codec headers and recomputes its ordinal from its selector with hashlib
- `system/lib/fuchsia-io/` — new library, scaffolded with copier: fuchsia.io's `Flags`, `Operations`, `VmoFlags`, `DirentType` and constants; the six methods `OPEN`, `CLOSE`, `CLONE`, `READ`, `GET_BACKING_MEMORY`, `READ_DIRENTS`; an encoder and a decoder for every request and every reply; `DirentWriter`, `DirentReader` and `reply_payload()`
- `system/lib/fuchsia-io/test/fuchsia_io_test.cpp` — 21 host tests: a round trip for every message and every malformed form refused
- `BUILD.gn` — `//system/lib/wire:tests` and `//system/lib/fuchsia-io:tests` join the default build on the host toolchain

**Symptom**
None; this is step 3 of the component-framework plan (`.agents/tasks/component-framework/PLAN.md`). Nothing in the tree speaks a file protocol yet. The vfs server library (step 4), the fsio client library (step 5) and the root (step 13) all speak this one, and the Launcher and DebugResource codecs (step 6) are built on the same framing.

**Cause**
A protocol between processes that do not trust each other is decided by its byte layout, and most of its safety comes from what the decoder refuses. Each rule below is there because breaking it gives a hostile peer something.

- **Why the header has a direction byte.** Take File.Read. The request is the header followed by `u64 count` at offset 16: 24 bytes. An Ok reply with no data, which is how end of file is reported, is the header, `u32 status = Ok = 0` at 16 and `u32 size = 0` at 20: also 24 bytes, with the same txid and the same ordinal. Read as a request, those last eight bytes are `count = 0`, so an EOF reply is a well-formed Read request. Now let a client make a channel pair, write one Read with count 0 into one end, and hand the root both ends, each as the `object` of an Open of some file. The root serves the Read and writes its reply, empty since the count is 0. The reply comes out of the other end, which the root also serves, and is read as a Read with count 0. The root replies to its own reply, and so on. The root has one thread (design D16), so every other component waits for ever. Zircon's `fidl_message_header_t` has no direction field because FIDL bindings never serve both ends of one channel as a server; the root, which serves whatever ends clients send it, cannot assume that. The direction byte takes the place of the first at-rest flag (byte 4). A server decodes only ToServer and a client only ToClient, so the two can never be confused (design invariant I5). The old launcher protocol closed the same loop with a reply ordinal whose top bit was set (`launch/protocol.hpp:44-50`); a byte in the header closes it for every protocol at once.
- **Why txids have rules.** A one-way request (Open, Clone) has no reply, so its txid must be 0; a two-way one must have a txid from 1 to `MAX_TXID`, and its reply must carry the same. A client checks the reply's txid against the request it sent, so a reply that comes late, or one meant for another request, is refused instead of being taken as the answer. The top bit is left alone, as Zircon reserves it for the txids the kernel generates in `zx_channel_call` (`kMinKernelGeneratedTxid`, `zircon/kernel/object/channel_dispatcher.cc:51`).
- **Why handles have placeholders, and why the count must match.** A channel message carries its bytes and its handles in two separate arrays. The body holds `HANDLE_PRESENT` (0xFFFF'FFFF) where a handle belongs, and the n-th placeholder names the n-th handle. A decoder requires exactly as many handles as the message has handle fields. If it ignored extra handles, a client could send a Read with fifty channel ends attached, and they would end up in the root's handle table with no field naming them and no code that knows to close them. Each one would then cost the root a slot until the root died.
- **Why every failure is the 24-byte status form.** A failed reply is `{header, status, 0}` and nothing else: no data, no handle. The encoder refuses a VMO next to an error, and the decoder refuses a failed reply that carries one. If a server could send a handle with a failure, a client that only looks at the status would never close it, and a bug on either side would leak capabilities without anything showing. The epitaph is the same 24 bytes, with txid 0 and the all-ones ordinal: the size of Zircon's `fidl_epitaph_t`. Open is one-way, so an epitaph on its `object` channel is the only way it can fail.
- **Why "malformed" and "failed" are kept apart.** A decoder's `Err` (always InvArgs) means the bytes are malformed. The caller drops the connection without an epitaph, as Fuchsia's Launcher does when a message fails to decode (`src/sys/lib/elf_runner/src/process_launcher.rs:92`, design "The protocol subset"). The server's own answer, such as AccessDenied, is the `status` field of a well-formed reply. The two need different reactions: a malformed reply means the peer cannot be trusted to speak the protocol at all, while a failed one is a normal answer. So a reply decoder returns `Result<ReadReply, Status>` with the server's status inside `ReadReply`, never one Status that could mean either.
- **Strict and flexible bits.** fuchsia.io declares `VmoFlags` strict and `Flags` flexible. Under FIDL's rules, a strict type with an unknown bit fails to decode, while a flexible one decodes and carries the bit to the server. The codec does the same: GetBackingMemory with bit 3 set is malformed, and Open with bit 62 set is well formed and left for the vfs to judge. A codec stricter than FIDL here would turn a server's choice into a framing error, and one looser than FIDL would let garbage reach the server.
- **What the codec does not judge.** An empty path, a path with a leading '/', a Read count above `MAX_TRANSFER_SIZE` and a combination like PRIVATE_CLONE with SHARED_BUFFER are all well formed. Fuchsia answers them with BadPath, OutOfRange and InvArgs from the server, so they decode here and the vfs answers them (step 4, D21). Only what cannot be decoded at all is malformed: a length that disagrees with the bytes, a path over `MAX_PATH_LENGTH` (FIDL's `string:N` bound is a decode error), a wrong handle count, non-zero padding, an unknown strict bit.
- **Why ordinals are hashes, and why a Python test checks them.** fidlc hashes each method's selector, `library/Protocol.Method`, with SHA-256 and keeps the first eight bytes, little-endian, top bit cleared. There is no central registry of numbers. A method that moves to another protocol keeps its number by keeping its old selector through `@selector`: Close moved into `fuchsia.unknown.Closeable` but still hashes `fuchsia.io/Node.Close`, Open was renamed from Open3 and still hashes `fuchsia.io/Directory.Open3`, and ReadDirents hashes `fuchsia.io1/Directory.ReadDirents`. The top bit is cleared so that no method can collide with the epitaph's all-ones ordinal. A mistyped ordinal fails silently on the target: the peer sees an unknown method and closes the connection, which looks the same as any other refusal. Since the tree has no SHA-256 in C++, `ordinals_test.py` recomputes each one with hashlib. It anchors its hasher to fidlc's own golden output, and it fails on any `wire::Method` declaration it cannot read, so a declaration written another way cannot pass unchecked.
- **Why a status outside `ours::Status` is refused.** In FIDL `zx.Status` is a plain int32, so any value decodes. In ours, `ours::Status` is an enum class that code switches over and passes to `to_string()`, so a decoder refuses any value at or past `Status::MaxCount` instead of passing on a number nothing names.

**Change**
The header, as FS §2.1 drew it:

```cpp
struct Header {
    u32 txid;
    Direction direction;          // byte 4: ToServer 0, ToClient 1
    std::array<u8, 2> reserved;   // zero
    u8 magic;                     // MAGIC = 1
    u64 ordinal;
};
static_assert(sizeof(Header) == 16);
```

Each method is declared once, its ordinal beside its selector, which is also what `ordinals_test.py` reads:

```cpp
inline constexpr auto READ = wire::Method{
    .selector = "fuchsia.io/File.Read",
    .ordinal = 0x057e'419a'298c'8ede,
    .kind = wire::MethodKind::TwoWay,
};
```

The bodies are flat, at fixed offsets from 16, with at most one trailing array whose length is the u32 before it (the table is in `fuchsia_io.hpp`). For example, Open is `u64 flags@16, u32 object@24 (handle), u32 path_size@28, char path[]@32`, and a Read Ok reply is `u32 status@16, u32 size@20, u8 data[]@24`. `MAX_PATH_LENGTH` is 1024 and `MAX_TRANSFER_SIZE` and `MAX_BUF` are 4072, so the largest Open and the largest Read reply both fit in one 4 KiB channel message.

Encoders write into a caller's buffers and return the `wire::Message` to write: the front of the byte buffer and the front of the handle buffer. A test can therefore pass an encoder's output straight to the decoder. `start_message()` zeroes the bytes before writing, so a reply buffer the root reuses never sends one client's bytes to the next (FS §3.3). A Read or ReadDirents reply may be built in place: `reply_payload()` gives the bytes from offset 24, and the encoder moves the payload with `memmove`, so data already there costs no second copy. This is the one copy in the root that D17 counts.

Library names, an open question of the design closed here: `system/lib/wire`, and `system/lib/fuchsia-io` as the plan names it, whose GN target is `fuchsia-io` so that the directory is its label. Its include prefix and namespace are `fuchsia_io`, since a C++ name cannot hold a hyphen; Fuchsia's own C++ bindings call the library `fuchsia_io::` too.

Rejected:
- A generated codec (FS §3, G3 and G4): S6 chose hand-written codecs.
- An `OutgoingMessage` type holding 4096 bytes and 64 handles: spans let the vfs size and place its own buffers, and returning `wire::Message` gives the same convenience.
- `Result<Status, Status>` at the fuchsia-io level, which could mean either "malformed" or "the server failed": each reply has its own struct carrying `status`. `wire::decode_reply_status()` and `wire::decode_epitaph()` keep that shape, since they are the framing's building blocks and say in their comments which is which.

**Reference**
- Zircon `fidl_message_header_t` (`zircon/system/public/zircon/fidl.h:430-438`), `FIDL_HANDLE_PRESENT` (`:326`), `kFidlWireFormatMagicNumberInitial` (`:422`), `fidl_epitaph_t`, 24 bytes, and `kFidlOrdinalEpitaph` (`:478-494`).
- fidlc `Sha256MethodHasher()` (`tools/fidl/fidlc/src/compiler.cc:28-45`); the anchor value comes from `tools/fidl/fidlc/goldens/anonymous.json.golden:316`.
- fuchsia.io: `Flags` (`sdk/fidl/fuchsia.io/directory.fidl:24`), `MASK_KNOWN_PERMISSIONS` and `MASK_KNOWN_PROTOCOLS` (`:140`, `:143`), `PERM_READABLE` (`:153`), `Open3` and `Open` (`:427`, `:442`; `openable.fidl:19`), `ReadDirents` (`directory.fidl:476`) and the dirent layout in its comment, `DirentType` (`:297`), `INO_UNKNOWN` (`:315`), `MAX_BUF` (`:293`); `Read` (`file.fidl:301`), `GetBackingMemory` and `VmoFlags` (`:192`), `MAX_TRANSFER_SIZE` (`:12`); `Operations` and `R_STAR_DIR` (`rights-abilities.fidl:9`, `:47`); `MAX_NAME_LENGTH` and `MAX_PATH_LENGTH` (`io.fidl:25`, `:50`).
- fuchsia.unknown: `Cloneable.Clone` (`sdk/fidl/fuchsia.unknown/unknown.fidl:21`), `Closeable.Close` with `@selector("fuchsia.io/Node.Close")` (`:39`); `fuchsia.io.Node` composes both (`node.fidl:295-297`).
- `docs/design/260924/userspace-filesystem.md` §2.1 (the framing) and §2.5 (strictness), as revised by component-framework D20 and D21.

**Verification**
- `host/obj/system/lib/wire/test/wire`: `[  PASSED  ] 11 tests.`
- `host/obj/system/lib/fuchsia-io/test/fuchsia_io`: `[  PASSED  ] 21 tests.`
- `python3 system/lib/wire/test/ordinals_test.py`: 5 tests, `OK`. Changing one digit of READ's ordinal makes it fail and name the selector and the right value.
- Mutation check: removing the direction check from `wire::decode_header()` fails 3 wire tests, and removing Open's exact-length check fails 2 fuchsia-io tests.
- Both suites also pass rebuilt by hand with `-fsanitize=undefined,bounds -fsanitize-trap=all` and `_LIBCPP_HARDENING_MODE_DEBUG`, which traps on any out-of-bounds `std::span` access. AddressSanitizer could not run: the prebuilt LLVM ships no `libclang_rt.asan_osx_dynamic.dylib`.
- Both sources also pass `-fsyntax-only` with the user toolchain's flags (`x86_64-unknown-none-elf`), where steps 4, 5 and 13 will build them; no user program links them yet.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed, 0 error lines), rel PASS (22 passed, 3 skipped, 0 failed, 0 error lines), the same as before the change.
- clang-format clean; clang-tidy reports nothing on the four sources or the two headers.

**Deliberately not done**
- No channel I/O: FS §3.3's `call`, `reply` and `send_epitaph`, and an `IncomingMessage` that closes unclaimed handles, need system calls and belong with their first user (vfs and fsio, steps 4 and 5).
- No handle type or rights checks: decoding sees handle values only. That needs `channel_read_etc`, which the kernel does not have (FS §2.5).
- No OnRepresentation, Seek, GetAttributes, ReadAt or Rewind: no step uses them (design non-goals).
- No unknown-ordinal policy: whether a fuchsia.io server answers an unknown method with an epitaph or just closes is the vfs's dispatch decision (step 4); the codec only offers `decode_header()` for dispatching on the ordinal.
- The Python test is run by hand, not by the build, like the gtests, which the build compiles but does not run.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- An empty File.Read reply and a File.Read request with count 0 differ in one byte. Which byte, and what would the root do, with one thread, if it were not checked?
- Why does GetBackingMemory refuse an unknown flag bit as malformed while Open passes an unknown bit on to the server, and who decided which is which?
- Close moved into `fuchsia.unknown.Closeable` and Open was renamed from Open3, yet their ordinals did not change. What makes that possible, and what would a client built before the move see if it had changed?
