# component changes

## Title: The compiled manifest is format version 3, and route() treats an expose and the declaration of a directory as hops, so that a provider can end a directory route (2026-10-30)

**Files**
- `system/lib/component/include/component/format.hpp` — `VERSION` is 3; `format::Capability` goes from 24 to 40 bytes (`reserved` becomes `flags`; `path` at 24 and `rights` at 32 are new); `format::Expose` goes from 40 to 56 bytes (`reserved` becomes `flags`; `subdir` at 40 and `rights` at 48 are new, so it has an `Offer`'s layout); the static_asserts pin every offset
- `system/lib/component/include/component/decl.hpp` — the decoded `Capability` gains `path` and `ustl::Option<Rights> rights`, the decoded `Expose` gains `subdir` and `rights`; the comment on `Decl` says what the decoder checks now
- `system/lib/component/src/decl.cpp` — `check()` of both records bounds-checks the new strings and puts `flags` and `rights` through `check_rights()`, the function a use already goes through; the two `reserved` checks go with the fields; `detail::view()` fills the new fields
- `system/lib/component/include/component/route.hpp` — an expose is a hop (`hops.apply(expose->rights, expose->subdir)`, AccessDenied or NotFound at the child, verb Expose); the declaration of a directory is the last hop (`hops.apply(capability->rights, {})`, AccessDenied at the provider, verb Declare) followed by a check that the declared path and the joined subdir fit one Open (NotFound at the provider, verb Declare); `Route` gains `path`; the spec of `route()` and its error list are rewritten
- `system/lib/component/src/route.cpp` — `detail::open_path_fits()`
- `system/lib/component/test/decl_test.cpp` — version 3, version 2 and 1 files refused, the new fields empty in every compiled fixture, a directory capability and expose patched in and read back, strings bounds-checked, rights and flags refused on both records, the byte sweep reaching the new strings
- `system/lib/component/test/route_test.cpp` — `Tree` takes a hook that patches manifests before the root resolves; 22 tests of `RouteDirectories` over leaf's `echo` in `top.cm`'s tree, patched into a six-record directory route
- `system/lib/component/README.md` — format version 3, the hops, the test notes

**Symptom**
Latent, cannot be triggered today: nothing writes a directory declaration or an expose that states rights (the compiler still refuses both, which is the next step), and the root still answers NotSupported for a directory use that routes to a provider (`realm.cpp`, a later step). What it prepares for is `.agents/tasks/directory-capability/DESIGN.md`: a filesystem is a component that provides a directory. The one visible trace before this change is a test. `ADirectoryUseIsNeverComparedAtAProviderEither` patches a protocol route into a directory route by hand and sees it end at the provider with no complaint, whatever the provider's side stated.

**Cause**
`route()` already carried the rule "stated rights only grow toward the source": walking from the use, every hop that states rights must contain the rights of the last hop that stated any, and the error names the hop that holds less. That rule is only as good as the list of hops it is applied to, and two places where a directory's rights can be stated were not on it:

- **The declaration at the end of the route.** A `Capability` had a kind, a name and a type, and no rights. A provider could not say "this directory allows at most r*", so an offer that asked for rx* of it was routed, and the first sign was whatever the provider's own server did at the Open. A ceiling that nobody checks is not a ceiling. The built-in `boot` already had one (`Builtin::rights`, applied as the last hop), so the missing piece was the provider's equivalent.
- **The expose.** An `Expose` had a source, a name and a target. A provider could pass its capability up to its parent and could not narrow it on the way: neither "only r*" nor "only the subdirectory `shared`". The comment in `route()` said so ("An expose states no rights and no subdir, so it is no hop for the rules below"), and the walk went through an expose without applying anything.

Neither could be added without the format. A hop that states rights needs somewhere in its record to say that it states them: a `u64` of zero is the empty set, not "none", and a route reads those two differently (a hop that states nothing narrows nothing, a hop that states the empty set allows nothing). An `Offer` and a `Use` already solve it with `RecordFlags::RightsStated`, in the word that was `reserved`. `Capability` and `Expose` had that word too, always zero and checked to be zero, so the flag takes its place and the layouts grow only at their ends: `path` (8 bytes) and `rights` (8) after a `Capability`'s `type`, `subdir` (8) and `rights` (8) after an `Expose`'s `target_name`. The first 24 bytes of a new `Capability` and the first 40 of a new `Expose` are the old record, which is why `Decl.RefusesAnOlderFile` can build a version-2 file from a version-3 one by cutting each record short. Every `u64` of the format still sits at a multiple of 8 within a record whose size is one (40 and 56), as the header of `format.hpp` requires.

The version is the other half of the change: the new records are bigger, so a reader that took a version-2 file for version 3 would read its second record from the middle of its first. The decoder refuses any version but `format::VERSION` before it looks at a count, and the writer and the reader are always built together, so a file of the old version cannot outlive its build.

**Change**
The two records, before and after:

```cpp
// before: 24 bytes                    // after: 40 bytes
struct Capability final {              struct Capability final {
    Kind kind;                             Kind kind;
    u32 reserved; // zero                 RecordFlags flags;
    String name;                           String name;
    String type;                           String type;
};                                         String path;    // new, at 24
                                           Rights rights;  // new, at 32
                                       };
// before: 40 bytes                    // after: 56 bytes, an Offer's layout
struct Expose final {                  struct Expose final {
    Kind kind;                             Kind kind;
    u32 reserved; // zero                 RecordFlags flags;
    Ref source;                            Ref source;
    String source_name;                    String source_name;
    Ref target;                            Ref target;
    String target_name;                    String target_name;
};                                         String subdir;  // new, at 40
                                           Rights rights;  // new, at 48
                                       };
```

The decoder puts the flags and rights of both records through `check_rights()`: unknown flags, rights stated without the flag, and unknown right bits are refused, as for a use. It does not check what the strings mean (that a directory declaration states a path and rights, that a protocol one states neither, that the path is absolute): that is the compiler's, and a declaration that states no rights narrows nothing.

In `route()`, two hops are added and one value is returned:

```cpp
// from the child: after the expose is found and is not from the parent
if (auto const status = hops.apply(expose->rights, expose->subdir); status != ours::Status::Ok) {
    return fail(status, &child, Verb::Expose, name);
}

// from self, a directory: the declaration is the last hop, then the Open must fit
if (auto const status = hops.apply(capability->rights, {}); status != ours::Status::Ok) {
    return fail(status, at, Verb::Declare, capability->name);
}
if (!detail::open_path_fits(capability->path, hops.subdir().view())) {
    return fail(ours::Status::NotFound, at, Verb::Declare, capability->name);
}
return found(at, capability->name, capability->path);
```

`Route::path` is the declared path, empty for a protocol and for a built-in, and kept apart from `Route::subdir`: the root will open the declared path without its leading `/`, then `/` and the subdir, in one Open. `open_path_fits()` counts exactly that, so the leading `/` is not counted. (The design's sentence says "a declared path, a `/` and the joined subdir", which counts it: that is one byte stricter than any Open needs, and `ThePathAndTheSubdirTogetherMustFitOneOpen` pins the exact count, 1012 bytes of subdir after `/export/data`.) The rights come first and the length second, as at every hop: a route that is denied and too long is denied.

The error is at the hop that holds less, as before. For an expose it is the child the walk turned down into, verb Expose, and the name is the one the child exposes the capability by; for the declaration it is the provider, verb Declare, and the name is the declared one. So `files-denied` of the design (an offer that asks rx* of an expose that states r*) is AccessDenied at the expose, and `files-wide` (an expose that states rx* of a declaration that allows r*) is AccessDenied at the declaration, and the two are told apart by the verb alone.

The protocol side does not change. A protocol is still compared by type at the end and never held to its declaration's rights, even if a manifest the compiler never wrote states some: `ADeclarationOfAProtocolIsNoHopAndStatesNoPath`. Nor does the directory side compare types: `BadTypeIsForProtocolsOnly`. The root (`realm.cpp`) is untouched and still answers NotSupported for a directory use that routes to a provider; that is step 4 of the design.

The rejected option for the declaration was to apply the hop with `hops.apply(capability->rights, {})` for both kinds. A protocol capability states no rights, so the call would do nothing today, but it would also make the day a malformed manifest states some a silent change of behaviour for protocols; the spec says a protocol is held to its type and nothing else.

**Reference**
Fuchsia, as `.agents/tasks/directory-capability/DESIGN.md` cites it: `ExposeDirectory` carries rights and a subdir (`sdk/fidl/fuchsia.component.decl/expose.fidl:130-136`); a directory declaration without rights is refused by the validator (`src/sys/lib/cm_fidl_validator/src/lib.rs:1379-1406`) and the request is held against them while routing (`src/sys/lib/routing/src/rights.rs:22-33`, `:108`); the provider's outgoing router joins `subdir` to `source_path` (`src/sys/component_manager/src/model/component/instance.rs:1682`). Linux: None, there is no capability routing.

**Verification**
- Host tests: `component` `decl` 20 to 26 tests, `route` 41 to 63, all pass; `python3 system/test_component_manifest.py` 99 to 102 tests (with `OURS_BUILD_DIR` at the build, so `FixtureRoutesTest` ran), all pass.
- Mutation checks, each made to a copy of the source, built, run and reverted: 12 on the decoder and 16 on `route()` (an expose that is no hop, a declaration that is no hop, the length checked before the rights, the path not returned, a protocol declaration made a hop, the leading `/` counted, rights held against the use, the kind ignored at a declaration, the failure reported at the parent, the type check applied to a directory, the length never checked, the program check dropped, the wrong verb, the expose subdir not joined, the path returned for a protocol) and 10 on the Python writer. Each is caught by at least one new test; none was missed.
- `boot_check.py --out out/dc-step1`, before the first edit and after: dbg 44 passed, 4 skipped, 0 failed, 0 error lines; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines; host tests 151 passed. The same counts.
- Every `.cm` the build wrote reads back as version 3 and has the size it should: the old size plus 16 bytes for each capability and 16 for each expose (`root.cm` 1369 to 1369, `echo.cm` 166 to 198, `routing_checks.cm` 1062 to 1062). The stripped `init` that the image packages went from 128,720 to 129,760 bytes (`init.image.elf`).
- clang-tidy on `decl.cpp`, `route.cpp`, `decl_test.cpp` and `route_test.cpp` against `out/dc-step1`: nothing on a line of this library; the output that remains is googletest's own header.

**Deliberately not done**
- The compiler does not write a directory declaration, an exposed directory or an offer of a directory from self or a child, and still refuses all three: `test_the_language_is_not_opened_by_the_format_change` keeps that true. Step 2.
- The root still answers NotSupported when a directory use routes to a provider, and `Route::path` has no reader yet. Step 4.
- No decoder check of the strings' meaning, as the design records.
- The guide quotes lines of `route.hpp` and `route.cpp` that moved; it is not edited here (`.agents/tasks/directory-capability-step1/RESULT.md` lists them).

**How it was found**
None. A new feature, from the design.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `Capability` keep its first 24 bytes and `Expose` its first 40 when the new format grows them, and what would have gone wrong if `rights` had been put before `type` instead of at the end?
2. A directory use asks r*, `top` offers it as rx*, and `realm` exposes it as r*: which hop does `route()` name, at which instance and with which verb, and why is it not the offer?
3. Why does the declaration's rights check come before the length check, and why does a protocol declaration that states rights not fail a protocol route?
