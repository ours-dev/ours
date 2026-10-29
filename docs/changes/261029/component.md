# component changes

## Title: The compiled manifest is format version 2: a Capability and a Use carry a `type` string, and a reader never sees it empty (2026-10-29)

**Files**
- `system/lib/component/include/component/format.hpp` — `VERSION` is 2; `format::Capability` gains `String type` (16 to 24 bytes) and `format::Use` gains `String type` before `rights` (48 to 56 bytes); the static_asserts pin the sizes and the offsets of `type` and `rights`
- `system/lib/component/include/component/decl.hpp` — the decoded `Capability` and `Use` gain `std::string_view type`
- `system/lib/component/src/decl.cpp` — `check()` bounds-checks the two new strings; `detail::view()` fills in the name when the stored string is empty
- `system/lib/component/include/component/route.hpp` — `Builtin` gains `std::string_view type = {}`, empty meaning the name
- `system/lib/component/test/decl_test.cpp` — the default, the explicit type, a version-1 file, a type string outside the table, and the every-byte sweep reaching the new strings
- `system/lib/component/test/fixtures/BUILD.gn` — `provider.cm`'s capability and `consumer.cm`'s use of `echo` state a `type`, so the decoder's test reads real bytes from the real template

**Symptom**
Latent, cannot be triggered today by this change alone: nothing reads the new field yet. What it prepares for is the experiment of 2026-10-29 in `.agents/tasks/protocol-identity/DESIGN.md`: `root.cm` offered the built-in `ours.process.Launcher` to `#pc-ps2` under the name `ours.resource.Irq`, the route resolved, the root said nothing, and pc-ps2 reported `getting the IRQ resource: PeerClosed`, which reads as a server that died.

**Cause**
A manifest says what a capability is called and nothing about what it is. `route()` matches a use to an offer, an offer to a declaration, by kind and name (`detail::find_offer()`), so a name is the whole identity of a protocol. That holds only while a capability keeps its name, and `as` on an offer exists to break exactly that: after `{ protocol = "ours.process.Launcher", as = "ours.resource.Irq" }` the name no longer says which protocol is behind it, and no record in the compiled manifest does either.

The fix needs a place in the file for the protocol's identity before any check can compare two of them, which is why this is the first of five steps and gives the field no meaning. A check written first would have nothing to read; a field added in the same step as the check would make the format change and the rule change one diff, and a failing boot would not say which of the two broke it.

**Change**
A Capability and a Use each carry a `String type`, so the two places where a protocol is stated can state its identity. An offer and an expose pass a capability on and carry none.

```cpp
// before: 16 bytes                       // after: 24 bytes
struct Capability final {                 struct Capability final {
    Kind kind;                                Kind kind;
    u32 reserved; // zero                     u32 reserved; // zero
    String name;                              String name;
};                                            String type;
                                          };
// before: 48 bytes, rights at 40         // after: 56 bytes, rights at 48
struct Use final {                        struct Use final {
    ...                                       ...
    String subdir;                            String subdir;
    Rights rights;                            String type;
};                                            Rights rights;
                                          };
```

`type` sits before `rights` in `Use` so the one `u64` stays last and on a multiple of 8, which the file's rule ("every u64 sits at a multiple of 8 within its record") needs: 12 words of 4 bytes, then 8. `Capability` has no u64, and 24 is still a multiple of 8.

An empty `type` string means "the entry's own name". The writer writes an empty string when the manifest omits `type`, so a manifest that never renames anything adds no byte to the string table; and an explicit type that spells the name adds none either, since the table stores each string once. The decoded view is where the meaning of "empty" is applied, once: `detail::view()` returns `type` as the name when the stored string is empty, so no reader has to remember the rule.

```cpp
// after: the Capability view, in decl.cpp
auto const name = strings.text(raw.name);
return {.kind = raw.kind, .name = name, .type = type_or_name(strings.text(raw.type), name)};
```

The design says the decoded records expose an accessor `type()`. They are aggregates the router copies out of the bytes one record at a time, so a field filled once is the same observable behaviour and needs no member function on a plain struct; this is recorded as a deviation in `.agents/tasks/protocol-identity-format/RESULT.md`.

The decoder checks `type` as it checks every other string (`check_string()`: the sum of offset and size is taken in 64 bits and must not pass the table), so an offset near 4 GiB cannot wrap back inside. `VERSION` goes to 2 and the decoder's existing test, `header.version != format::VERSION`, refuses a version-1 file with NotSupported, before any record is read: read as version 2, a version-1 file's smaller records would be taken for others. No compatibility is kept. The writer and the reader are built together and a manifest never outlives its build, so a file of the older version cannot exist outside a stale build directory.

`Builtin::type` is the root's third place to state an identity, for the protocols it provides itself. It is last in the struct, with a default of `{}` meaning the name, so every designated initialiser in `realm.cpp` and `route_test.cpp` stays as it is, and no hand-made list needs to change.

Rejected: adding the field to Offer and Expose too. An offer renames a capability, not its protocol; a type there would be a second place that could disagree with the declaration it points at.

**Reference**
None, this project's own design. The nearest system, Fuchsia's `cm_rust`, has no type at all: `UseProtocolDecl` has a `source_name` and `ProtocolDecl` a `name`, so a protocol is its name (`src/sys/lib/cm_rust/src/use.rs:66`, `capability.rs:54`). `.agents/tasks/protocol-identity/DESIGN.md` records why ours departs from it: `as` makes the name differ from the protocol on purpose.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then both run from `out/bringup.x86`: `decl` 20 tests passed, four of them new (`ATypeIsWhatTheManifestStatedOrTheName`, `AStoredTypeIsReturnedAsStored`, `RefusesAVersionOneFile`, `RefusesATypeOutsideTheTable`), `RefusesABadHeader` now refusing versions 1 and 3 where it refused 2, and `Provider`, `Consumer` and the every-byte sweep reading the new field; `route` 32 tests passed unchanged.
- `RefusesAVersionOneFile` builds a faithful version-1 file from a fixture (the same tables without the two `type` strings, version 1 in the header, `total_size` corrected) rather than only patching the version word, so it is a file the old writer could have written.
- clang-tidy on `decl.cpp` and `decl_test.cpp` against `out/bringup.x86`: no diagnostic outside `vendor/googletest`. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean on every changed file.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-format`: build ok in 6.6 s; dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines, `routing_checks: passed` among them; host tests PASS, 148 passed, 0 failed. Every `.cm` of both images and of the fixtures reads back as version 2 (`magic 0x4d43534f`, `version 2`), so no manifest is still written at version 1.

**Deliberately not done**
- No comparison of types anywhere: `route()` does not read `type` or `Builtin::type`, and a mismatched route still resolves. That is steps 2 (run time) and 3 (build time) of the task.
- No helper for `Builtin::type` yet. Step 2 reads it through one function so that a list with the field left empty keeps working; with no reader there is nothing to put the helper next to.
- `system/lib/component/README.md` and the architecture documents still describe the format without the field. They are rewritten once, in step 5, when the checks they would describe exist.
- No `type` on a directory use. A directory's hops are compared by rights at every step, and the only directory is the built-in `boot`; a type there would be a field nobody reads.

**How it was found**
None. It is the first step of the plan accepted on 2026-10-29 for the experiment recorded in `.agents/tasks/protocol-identity/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `Use` put `type` before `rights` and not after it, and what rule of the file would break if it did?
2. A manifest states `type = "ours.example.Echo"` on a capability that is itself named `ours.example.Echo`. What does the writer put in the string table for it, and how does the decoded record differ from the same manifest with no `type`?
3. Why is a version-1 file refused by its version word and not left to fail the size check, and what would a reader of the new layout make of a version-1 `Use` if it got that far?
