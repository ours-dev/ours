# ours-acpi changes

## The codec of ours.acpi.Route, the protocol of the ACPI route service (2026-10-31)

**Files**
- `system/lib/ours.acpi/include/ours_acpi/ours_acpi.hpp`, `src/ours_acpi.cpp` — new: the protocol `Route` with `IsaIrq` and `EcamBase`, each request and reply with its encoder and decoder, the two selectors and their ordinals.
- `system/lib/ours.acpi/BUILD.gn`, `test/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — new, as `ours.resource` has them.
- `system/lib/ours.acpi/test/ours_acpi_test.cpp` — new, 14 cases.
- `system/lib/wire/test/ordinals_test.py` — the two selectors, so the test recomputes their ordinals.
- `BUILD.gn` — the test group joins the default build.
- `.agents/tasks/acpi-route/PLAN.md` — decisions D1 to D3 recorded.

**Symptom**
Latent, cannot be triggered today: the route service's logic answers two questions (`docs/changes/261031/acpi-route.md`), and nothing yet lets a program ask them. A caller and the program that answers need the same bytes on a channel, written once.

**Cause**
Not a mistake: a missing piece, and the way it is built is decided by what a channel gives. Everything in `ours.resource`, `ours.io` and the echo example is encoded by hand on `//system/lib/wire`, because the tree has no generator, and a method's number is the hash of its name so that it follows from the name. Four details decide what this one says.

- **Flat bodies at fixed offsets.** The header takes 16 bytes and the body follows, so a request is `isa_irq` or `segment` at 16, and an Ok reply puts its status at 16 and its fields after. The layout is written in the header's comment and again as raw offsets in a test that reads the bytes directly, since a codec that encodes and decodes with the same wrong constant round-trips without error.
- **Exact sizes.** A decoder refuses a message one byte short, a word long, or with a handle, and a reply that is not Ok must be the 24-byte status form with nothing after it. A reader that accepted trailing bytes would let two programs disagree about where a message ends.
- **The wire has its own enums.** `Trigger` and `Polarity` are defined here, with values that never change, and not borrowed from the service's library, so that a caller does not link against the service to read an answer; the service maps between them. An encoder refuses a value they do not name, and a decoder refuses one it receives.
- **Narrow fields in wide words.** A segment is 16 bits and is carried in a 32 bit word whose upper half must be zero; the two bytes after `last_bus` in the ECAM reply are a reserved word that must be zero. A value in a field that is meant to be empty is how a format grows later without telling anyone, so it is refused now.

**Change**
The two ordinals are the first 8 bytes of SHA-256 of the selector, `ours.acpi/Route.IsaIrq` and `ours.acpi/Route.EcamBase`, computed as the other methods' are and checked by `ordinals_test.py`. The layout:

```text
Route.IsaIrq    request  u32 isa_irq@16                                  20 bytes
                reply    u32 gsi@20, u32 trigger@24, u32 polarity@28     32 bytes
Route.EcamBase  request  u32 segment@16 (upper half zero)                20 bytes
                reply    u8 first_bus@20, u8 last_bus@21, zero u16@22,
                         u64 address@24, u64 size@32                     40 bytes
```

**Reference**
`ours.resource` (`system/lib/ours.resource`) and the echo protocol (`examples/echo/protocol`) in this project, whose encoders and decoders this follows, and `wire::start_request()`, `wire::start_ok_reply()` and `wire::decode_reply_status()` that it builds on. None else: the protocol is this project's own design (`.agents/tasks/driver-manager/DESIGN.md`).

**Verification**
- `ours_acpi` (new, 14 gtest cases): each request and reply round-trips, including the extremes (a GSI of 0xffffffff, every trigger and polarity, a segment of 0xffff, an address and a size of all ones); every refusal an encoder makes (a txid of 0 or past the maximum, no room, a body next to a failure, a value the enums do not name) and every one a decoder makes (a short or long message, a handle, the other method's message, the wrong direction, a txid that is not the request's, an Ok reply in the status form, a value out of an enum, a nonzero reserved word, a failure with a body after it); and the raw bytes at the offsets above.
- Fifteen mutations, each caught by a test and each restored byte for byte: the size or the handle unchecked in a request, the segment width unchecked, a body accepted next to a failure (both replies), the enums unchecked in the encoder and in the decoder, the size or the handle unchecked in a reply, the reserved word unchecked, the trigger, the buses and the size stored at another field's offset, and one method's ordinal used for the other. Three were first written so that they did not compile (the compiler's own unused-variable check caught the first, and the layout's `static_assert`s the two that moved a constant), and were rewritten to change the code instead.
- `ordinals_test.py` OK. Compiled for user space with the flags of a real user-space source (`-ffreestanding`, `-fno-exceptions`, `-fno-rtti`, `-Werror`); clang-tidy: no findings.
- `boot_check.py`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host tests 154 passed (153 before: the new executable). For user space it needs only `memcpy` and five functions of `//system/lib/wire`, which every user program links.
- Not verified: no program sends or receives these messages yet, so how they behave on a real channel is untested; that is the next change.

**Deliberately not done**
No client and no server: a client needs a channel and the namespace, and both are the next change with the program that uses them. No method for `_PRT` or `_CRS` (they need AML). The service's own `InterruptRoute` and `EcamWindow` are not reused as the wire types, so that the wire stays what the codec says and the service can change its types.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A codec encodes the IsaIrq reply with the trigger at offset 28 and decodes it from 28. Why does a round-trip test not notice, and what does?
- Why must the status-form reply of a failure have nothing after it, and what would two programs that disagree about that do?
- Why does the wire have enums of its own when the service's library already has a `Trigger` and a `Polarity`?

## The client: `isa_irq()` and `ecam_base()`, one request on a connection of its own (2026-10-31)

**Files**
- `system/lib/ours.acpi/include/ours_acpi/client.hpp`, `src/client.cpp` — new.
- `system/lib/ours.acpi/BUILD.gn` — the `client` target, for user programs only.

**Symptom**
Latent, cannot be triggered today: the codec had nobody to speak it.

**Cause**
Not a mistake: a program that asks the route service needs the same few steps each time, and they are the ones `ours_resource::get()` has: build a path under `/svc`, connect through the namespace, write one request, wait for a reply or the peer closing, read it, and tell an answer from an epitaph. An epitaph is what a route that leads nowhere closes the connection with, and a component that has exited is one such route, so a client that took it for a malformed reply would hide NotFound as InvArgs.

**Change**
Two functions, each opening a connection, writing one request, reading one message, decoding it and closing the connection. A reply whose status is not Ok is the function's Err, so a caller asks "is there a line" with `is_ok()`; the codec's replies keep the status inside the reply for a server to write.

**Reference**
`ours_resource::get()` in `system/lib/ours.resource/src/client.cpp`, whose steps these follow; None else.

**Verification**
By the program that uses it: `check_acpi_route()` in routing_checks goes through it for every answer (`docs/changes/261031/acpi-route.md`). clang-tidy: no findings. Not tested alone: a client needs a channel and a namespace, which only a booted image has.

**Deliberately not done**
No connection kept between calls: each call costs a connection, and a driver asks once or twice. No timeout, as `ours_resource::get()` has none.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a reply with a failure status the function's Err, when the codec returns it as a reply?
- What would a client that took an epitaph for a malformed reply tell its caller when the service has exited?
- Why does the client read a message into a buffer that has room for a handle?

## The codec's tests hold the txid rule and the width of an enum; the contract says what the codec does not check (2026-10-31)

**Files**
- `system/lib/ours.acpi/test/ours_acpi_test.cpp` — requests with a txid the rule refuses, the wrong direction for EcamBase, and enum fields whose low byte is a valid value.
- `system/lib/ours.acpi/include/ours_acpi/ours_acpi.hpp`, `client.hpp` — what a reader of an EcamBase reply must check, and NoResource among the client's errors.
- `system/lib/ours.resource/test/ours_resource_test.cpp` — `ACPI_PROTOCOL`, and the two protocols the list had left out, join the per-protocol tests.

**Symptom**
Found by the independent review: a mutant of the codec that dropped the txid rule passed all 14 tests, and so did one that cast an enum field to a byte before comparing it, so that a trigger of 0x100 decoded as Edge.

**Cause**
The txid rule lives in `wire::decode_request_header()`, which the wire library tests, so a codec that calls it is right, and a codec that stopped calling it was held by nothing. The test now sends the txids the rule refuses. The enum fields are 32 bits wide, and a decoder that looked at the low byte accepted a value no encoder writes; the test now sends values whose low byte is valid. The EcamBase reply carries a window whose parts have relations (the first bus is not after the last, the size is the buses times one MiB, the address and size do not wrap) that the codec does not check, deliberately, as it checks framing; the contract now says so, and that the reader maps the window only after it has checked them, as the service does before it answers.

**Change**
Tests, and comments. The library's own per-protocol list gains Sampling, `ours.kernel.RootJob` and `ours.boot.Acpi`, which share the shape of its other protocols and were not in it.

**Reference**
None, this project's own design.

**Verification**
`ours_acpi` (14 cases) and `ours_resource` (6) pass; two mutations of the codec caught (the txid rule dropped, an enum read as a byte), restored. A first run of the new request test failed on its own bug: the loop that set a bad txid left the last one in the message the later checks reused; it restores the txid after the loop. A binary that a failed build had left behind also let a stale run pass once, so the build's result is read before a test is.

**Deliberately not done**
The codec does not validate the window. A server that wanted to would put the checks in the encoder too.

**How it was found**
By the independent review, with mutants of the codec run by the skeptic.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A test that sends only valid and obviously invalid values misses a decoder that compares one byte of a 32 bit field. Which values catch it?
- Why is the txid rule tested in the wire library and still worth a case in the codec's tests?
- What must a client do with an EcamBase reply before it maps the window?
