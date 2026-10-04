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
