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

## Title: route() answers BadType, with both types, when a route ends at a capability of another protocol type (2026-10-29)

**Files**
- `system/lib/component/include/component/route.hpp` — `RouteError` gains `want` and `got`; `route()` compares the use's `type` with the type of the end of the walk, once, for a protocol; `detail::builtin_type()` is declared; the spec comment of `route()` gains "The type check", the BadType error, and the new invariant and exclusion; `Builtin::type`'s comment says it is now read
- `system/lib/component/src/route.cpp` — `detail::builtin_type()`: the built-in's stated type, or its name when it states none
- `system/lib/component/test/route_test.cpp` — nine tests in `RouteTypes`, a third tree (`mismatch.cm`), the BadType line of `describe()` and four small helpers that break one manifest on purpose
- `system/lib/component/test/fixtures/BUILD.gn` — `mismatch.cm`, built with `type_check = false`; user.cm and leaf.cm state the `type` of the renamed protocols they use
- `system/lib/component/test/BUILD.gn` — `mismatch.cm`'s bytes are embedded as `MISMATCH`
- `system/lib/component/README.md` — the router test routes three trees

**Symptom**
The experiment of 2026-10-29 in `.agents/tasks/protocol-identity/DESIGN.md`: `root.cm` offered the built-in `ours.process.Launcher` to `#pc-ps2` as `ours.resource.Irq`. `route()` answered a Route to the Launcher, the root logged nothing, and pc-ps2 read `PeerClosed` from a server that had closed the channel on a method it did not know. Before this change the same walk through the router's test would have answered `provider serves ours.example.Echo` for a use that meant `ours.process.Launcher`; after it, it answers `BadType at provider: Declare ours.example.Echo (wants ours.process.Launcher, got ours.example.Echo)`.

**Cause**
The walk matched by kind and name at every step, and a name is all a route carries: `use.source_name` goes up through the offers (each may rename with `as`), down through the exposes, and ends at whatever declaration has the name it arrived with. Nothing in the walk knew what protocol the use meant or what protocol the declaration provides, so reaching a declaration at all was taken for success.

Step 1 put the identity into the file (`Capability::type`, `Use::type`, `Builtin::type`). This step is the first thing that reads it, and it reads it at one place only: where the walk ends. An offer and an expose carry no type, because they do not make a protocol, they pass a capability on under a new name; comparing at each hop would compare strings the offers do not own. So the comparison is one `!=` of two string views after the walk has found the provider or the built-in.

The order of the checks at the end matters, and is why the answer is not "compare as soon as the name is found":
- A provider without a program, or without a declaration of that name, is NotFound whatever its type says. A declaration that does not exist or cannot be served has no type worth comparing; reporting BadType there would send the reader to fix a type when the real fault is a missing program.
- A built-in goes through its hop first. A built-in directory has rights and a protocol built-in may be given some, and `Hops::apply()` answers AccessDenied when the built-in holds less than the use asked for. That is a fault in who may have what, and it must keep its status; the type is compared after the hop passes.
- A directory use is never compared. A directory is held to its rights at every hop and the only one is the built-in `boot`; `Use::type` of a directory use is the name that `detail::view()` filled in, which would differ from `boot` for every renamed directory (`config`, `narrow`) and make every one of them a mismatch. The guard is `use.kind == Kind::Protocol`, on both ends.

The status is BadType, which already means "a thing of the wrong type" (`ours::Status`), and the verb is Declare, the declaration the walk reached, so no new status (a change to the system call ABI) and no new verb are needed. The two types ride in the error as views (`want`, `got`) rather than being formatted into it: the root has no heap, the log line (step 4) formats them where it prints, and the views point into the manifests' bytes or the built-ins, like `name`, so they cost nothing.

The existing fixtures turned out to depend on the gap. `user.cm` used `ours.example.RealmEcho` and `leaf.cm` used `echo` with no type, and both resolve to provider's `ours.example.Echo` through renames. With the comparison in, every one of those routes in `Route.TwoLevelsUpAndTwoDown` answers BadType, and the build check of step 3 would refuse the trees. They now state `type = "ours.example.Echo"`, as `consumer.cm`'s `echo` did since step 1, each in a `use` entry of its own because a type goes with one protocol and not a list. `mismatch.cm` is the fixture that is wrong on purpose.

**Change**
`route()` ends two ways, at a built-in and at a provider, and each now compares before it answers:

```cpp
// before: the end of the walk, at a built-in
if (auto const status = hops.apply(builtin->rights, {}); status != ours::Status::Ok) {
    return fail(status, nullptr, Verb::Declare, name);
}
return found(nullptr, builtin->name);

// after
if (auto const status = hops.apply(builtin->rights, {}); status != ours::Status::Ok) {
    return fail(status, nullptr, Verb::Declare, name);
}
auto const type = detail::builtin_type(*builtin);
if (use.kind == Kind::Protocol && type != use.type) {
    return mismatch(nullptr, builtin->name, type);
}
return found(nullptr, builtin->name);

// before: at a provider
return found(at, capability->name);

// after
if (use.kind == Kind::Protocol && capability->type != use.type) {
    return mismatch(at, capability->name, capability->type);
}
return found(at, capability->name);
```

`mismatch` builds `RouteError{.status = BadType, .at = at, .verb = Verb::Declare, .name = name, .want = use.type, .got = got}`. `want` and `got` default to empty, so every other construction of a `RouteError`, the root's included (`realm.cpp` reads `status`, `at`, `verb` and `name`), compiles and means what it meant. `detail::builtin_type()` is the one reader of `Builtin::type` and lives in `src/route.cpp` so that a program carries one copy of "empty is the name", however many instance types it routes over.

Nothing is allocated and no system call is made: the comparison is two `std::string_view` equalities, run on every Open of a `/svc` entry (D11).

Rejected: comparing in each offer and expose of the walk. They carry no type, so the walk would have to look the end up first, and then it would be the same comparison made later than it can be. Rejected: comparing before the built-in's hop, which makes a route that is both wrong in type and denied report the type; the test `ARouteThatFailsForAnotherReasonKeepsItsStatus` fails when the two are swapped, which was checked.

**Reference**
None, this project's own design. The nearest system, Fuchsia's component framework, matches a use to an offer by name and has no type to compare (`.agents/tasks/protocol-identity/DESIGN.md`, "Problem"); the design departs from it because `as` makes the name differ from the protocol on purpose.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then each run from `out/bringup.x86`: `route` 41 tests passed (32 before, nine new in `RouteTypes`; none of the 32 changed), `decl` 20 tests passed.
- Each guard was removed in turn to see its test fail: with `use.kind == Kind::Protocol &&` taken out of the provider's end alone, `ADirectoryUseIsNeverComparedAtAProviderEither` fails and the rest pass; with both taken out, `ADirectoryUseIsNeverCompared` fails too, and so do the three tests of subdirs, whose directory uses (`config`) would otherwise be compared against `boot`. With the built-in's comparison moved ahead of its hop, `ARouteThatFailsForAnotherReasonKeepsItsStatus` fails and the rest pass. The tree was restored after each.
- clang-tidy against `out/bringup.x86` on `src/route.cpp` and `test/route_test.cpp` (route.hpp reaches both): no diagnostic outside `vendor/googletest`. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean on the changed lines and files.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-runtime`: build ok in 6.3 s, which includes `system/service/init` (its two calls of `route()` and its `RouteError` logger); dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines (`routing_checks: passed` among them); host tests PASS, 148 passed, 0 failed. The real image runs the comparison on every Open of every `/svc` entry: no `init:` line in either log reports a route that leads nowhere other than routing_checks' two deliberate ones (`denied`, `ours.test.Unoffered`), so the root's built-ins, `routing_checks`' renamed Echo and every other protocol use compare equal, and there is no false positive.

**Deliberately not done**
- `realm.cpp`'s `log_route_error()` does not name the types. A BadType today logs as `init: <moniker>'s use of "<name>" routes nowhere: BadType at <where> (declaration of "<name>")`, which is correct and says less than it could; the line that names `want` and `got` is step 4.
- No build-time check. `type_check = false` on `mismatch.cm` is accepted and inert until step 3 gives the build a check to skip.
- No type on a directory. A directory's hops are compared by rights.
- No new `Status` and no new `Verb`.
- Not every `Builtin` of `realm.cpp` states a type: they state none, which means their names, and every use of them uses its own name.

**How it was found**
None. It is the second step of the plan accepted on 2026-10-29. The dependence of `user.cm` and `leaf.cm` on the gap showed as soon as the comparison was in: `Route.TwoLevelsUpAndTwoDown` failed with `BadType at realm/provider: Declare ours.example.Echo (wants echo, got ours.example.Echo)` before the two uses stated their type.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A built-in states rx* for a protocol and the use asks r*, and the built-in's type also differs from the use's. What does `route()` answer, and which two lines of the walk decide it?
2. `config` is a directory use renamed from `boot`. Why would it be a mismatch if the comparison had no `use.kind == Kind::Protocol` guard, and what value does the decoded `Use::type` of a directory use hold?
3. Why is the comparison made once, where the walk ends, and not in each offer and expose the walk passes through?
