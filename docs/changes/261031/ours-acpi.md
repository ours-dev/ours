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
