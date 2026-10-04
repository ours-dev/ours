# fsio outstanding repairs

## 1. A File.Read copies its bytes four times, and every fsio call zero-fills about 8 KiB first

**Severity**
Low: a performance cost, not a fault. Nothing reads files through fsio yet; routing_checks (step 13) and the shell's `ls` (step 15) will, a few KiB at a time.

**Symptom**
Not measured. On the path of one `fsio::read()` of n bytes:
- the root copies them from its BootFS mapping into its reply buffer (design D17);
- `channel_write` copies the reply into the kernel;
- `channel_read` copies it into fsio's `Incoming` buffer;
- fsio copies the payload into the caller's buffer (`copy_payload()`, `system/lib/fsio/src/fsio.cpp`).

Before any of that, each call value-initialises its buffers: `Incoming` holds 4096 bytes and 64 handles, and `open()` also clears a 4096-byte request buffer, which `wire::start_message()` then clears again for the bytes it uses.

**Cause**
The fourth copy exists because `channel_read` fills one buffer, and the payload starts 24 bytes into it, behind the header, the status and the size. Fuchsia pays the same copy: zxio reads into a stack `SyncClientBuffer` and `memcpy`s the data out (`RemoteReadv`, `sdk/lib/zxio/remote.cc:37-61`). The zero-fill comes from `std::array<...> m{}` members, written so that no byte of an earlier message can be read by mistake. `Incoming` only ever exposes the bytes `channel_read` wrote, so for it the fill buys nothing.

**Change**
Measure first, as CLAUDE.md asks for any performance work: cycles per `fsio::read()` of 64 B, 1 KiB and 4072 B against the root's /boot, split into the four copies and the two fills, with the system-call cost beside them for scale. Candidates, for the user to choose between with the numbers:
- leave `Incoming`'s buffers uninitialised, which removes the fill at no risk, since only the first `actual_bytes` are ever read;
- a `read()` overload that takes a caller's 4 KiB message buffer and returns a span into it, which removes the fourth copy for callers that can use the data in place;
- for bulk reads, `get_backing_memory()` and a mapping, which D17 already names as the path for programs and bulk readers.

**Reference**
zxio `RemoteReadv()` (`sdk/lib/zxio/remote.cc:37-61`), which makes the same copy; component-framework design D17.

**Acceptance**
A benchmark in `benches/` reports the per-call cost before and after; the user has picked which candidate, if any, to apply; `system/lib/fsio` host tests and `ours-verify` pass.
