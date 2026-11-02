# component-manifest changes

## Title: The compiler writes format version 3, with a Capability of 40 bytes and an Expose of 56, and leaves the new fields empty (2026-10-30)

**Files**
- `system/component_manifest.py` — `VERSION` is 3; `CAPABILITY` is `<IIIIIIIIQ` (40 bytes: kind, flags, name, type, path, rights at 32) and `EXPOSE` is an `OFFER`'s `<IIIIIIIIIIIIQ` (56 bytes: subdir at 40, rights at 48); the dataclasses `Capability` (`path`, `rights`) and `Expose` (`subdir`, `rights`) gain the fields with empty defaults; `encode()` writes them
- `system/test_component_manifest.py` — the sizes and the version, the fields at the offsets `format.hpp` fixes (read without the structs that wrote them), every compiled capability and expose with the new fields empty, a directory capability and a directory expose built by hand and read back, and the three refusals kept

**Symptom**
Latent, cannot be triggered today: no manifest the language can write states a path, a subdir or rights on a capability or an expose, so every new field is written empty and the only visible change is that every `.cm` is 16 bytes bigger for each capability and each expose it holds and says version 3. What it prepares for is the compiler change of the next step, which lets a manifest say them; without this step that change would be a format change and a language change in one diff, and a failing boot would not say which of the two broke it.

**Cause**
The format has two writers' halves and they must agree byte for byte: `format.hpp` and the Python `struct` strings, each with a twin of every constant. `component::format::Capability` and `Expose` grew (see `docs/changes/261030/component.md`), so the writer has to grow with them or the C++ decoder reads the second record of every table from the middle of the first. The decoder refuses a version-2 file, so a writer left at 2 would not even boot: `init` would fail to decode `root.cm`.

An empty string costs no byte. `_Strings.add("")` records the offset of the end of the table so far and size 0, and a string that is already in the table is not added again, so a capability that states no path adds no byte to the string table: `test_an_omitted_type_writes_an_empty_string` still sees the table `b"pq/svc/q"`. The only growth of a `.cm` is therefore the records: 16 bytes for each capability and 16 for each expose.

**Change**
```python
# before                                               # after
CAPABILITY = struct.Struct("<IIIIII")  # 24            CAPABILITY = struct.Struct("<IIIIIIIIQ")  # 40
EXPOSE = struct.Struct("<IIIIIIIIII")  # 40            EXPOSE = struct.Struct("<IIIIIIIIIIIIQ")  # 56, an Offer's layout
```

`encode()` writes the flags and rights of a capability and an expose through the same `rights()` helper a use and an offer go through: `None` writes no flag and a zero word, any number writes `FLAG_RIGHTS_STATED` and the mask, so `0` is a stated empty set and not an unstated one (`test_a_directory_capability_and_expose_encode_at_the_offsets_format_hpp_gives` pins the difference). The `Capability` and `Expose` dataclasses take the new fields as trailing defaults, so every existing construction, positional ones in the tests included, is unchanged and compares equal to what the parser builds.

The language is not opened. `compile_manifest()` still refuses a declared directory, an exposed directory and an offer of a directory from self or a child, and the parser never sets the new fields. `test_the_language_is_not_opened_by_the_format_change` pins that with the full spelling the design uses (a directory with `path` and `rights`, an expose with `rights` and `subdir`, an offer from `self` and from a child), so a later step that removes a refusal has to change that test on purpose. The rejected option was to write the parser's half now, the keys and the path rules, with the format change: it is the next step, with its own tests and its own boot.

**Reference**
None for the writer, which is this project's own half of a contract; the decoder's change record names the Fuchsia counterparts of the fields.

**Verification**
- `python3 system/test_component_manifest.py` with `OURS_BUILD_DIR` at the build: 102 tests (99 before; two replaced, five added), all pass, `FixtureRoutesTest` included, which compiles the fixtures through the same `encode()`.
- The C++ `decl` test decodes every fixture this script wrote (`realm.cm`, `provider.cm`, `consumer.cm`, `hollow.cm`) in version 3, so the two halves are checked against each other, not only each against its own table.
- Mutation checks on the writer, each made to a copy, run and reverted: the version left at 2, the type and path swapped, the rights not written, the capability flags not written, the expose flags not written, the expose subdir written as the target name, the expose rights not written, the old 40-byte expose layout, the old 24-byte capability layout, and rights placed first. Each is caught: the two layout mutations by eight and eleven tests, the version by one, the others by one to three.
- `boot_check.py --out out/dc-step1`: dbg 44 passed, 4 skipped, 0 failed; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed; the same as before. All 15 `.cm` files of the build read back as version 3 with the sizes above.

**Deliberately not done**
The parser's keys (`path` and `rights` on a directory declaration, `directory`, `rights` and `subdir` on an expose), the path rules, the non-overlap rule and the build-time rights walk: the next step. No manifest of this repository changes.

**How it was found**
None. A new feature, from the design.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `encode()` write a stated empty set of rights differently from none, and who reads the difference?
2. Why can the writer and the decoder not be changed one after the other, as two commits?
3. A capability that states no path adds no byte to the string table: why, and what in the tests would notice if it did?

## Title: A component declares, exposes and offers a directory, and the build warns for a directory route that holds less than its use asks (2026-10-30)

**Files**
- `system/component_manifest.py` — `capabilities` takes `directory` with `path` and `rights`; `expose` takes `directory` with optional `rights` and `subdir`; `offer` of a directory comes from `parent`, `self` or `#child`; the path rules and the non-overlap rule; the three refusals and their D3 texts go; the rights walk (`route_directory_use`, `check_directory_rights`, `format_rights_warning`) and its call in `main()`
- `system/component_manifest.gni` — the header documents the directory language, the warning, and that `type_check = false` silences both walks
- `system/test_component_manifest.py` — every rule accepted and refused by the message written for it, the bytes read back, `test_the_language_is_not_opened_by_the_format_change` replaced by its opposite, the walk, the warning text, the command line, and the cross-check against `route_test.cpp` on the new fixtures
- `system/lib/component/test/fixtures/BUILD.gn` — a directory tree: `shelf.cm` (root), `store.cm` (provider), `reader.cm` (consumer), `bare.cm` (a provider with no program)
- `system/lib/component/test/BUILD.gn` — the four manifests are embedded
- `system/lib/component/test/route_test.cpp` — `RouteShelf` tests over the compiled fixtures, in place of the patched-bytes tests that covered the same ground
- `system/lib/component/test/decl_test.cpp` — the decoder reads the compiled fixtures' paths, subdirs and rights; the patched `directory_provider()` is gone

**Symptom**
A manifest that declared a directory failed the build with "a component cannot declare a directory; the only one is the root's built-in `boot` (D3)", and an exposed directory and an offer of a directory from `self` or a child with the same limit in other words. Nothing else changed for a real manifest: no manifest of the dev image states a declared directory, so every one of the six it packages (`root`, `echo`, `hello`, `shell`, `pc-ps2`, `routing_checks`) compiles to the same bytes as before (compared by running the compiler of the previous commit and this one on the same inputs).

The second symptom is a signal that did not exist. A directory use whose offer states fewer rights than the use asks for is found by the root when it starts the component: it logs one line and closes the entry with an AccessDenied epitaph. The build said nothing, and ninja shows an action's output only when the action runs, so a reader of the log had no way to learn it before booting. Now `root.cm` prints, each time its action runs, the warning below for routing_checks' deliberate `/denied`:

```text
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
           use    #routing_checks "denied"                                    rx*
           offer  root.cm "boot" from parent to #routing_checks as "denied"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.
```

That warning is expected and will stay until a marker that says "this failure is meant" exists (the design's open question). The exit status is 0 and `meta/root.cm` is written, byte for byte what it was.

**Cause**
D3 fixed "protocol capabilities only, the only directory being the built-in `boot`" because no component then had a directory to provide, and every extra source in the language was a rule to write and a case to test for a use nothing had. The compiler enforced it with three refusals (a declared directory, an exposed directory, an offer of a directory from anything but the parent), and the router (`route()`) did not need them: it matches the kind at every lookup, so a directory walks through offers and exposes as a protocol does. Step 1 gave the format and the router the two missing hops, the declaration and the expose. This step takes the refusals out of the compiler and writes what the language now says.

Why the rules for a declared path are the ones they are:

- The path names a place in the provider's outgoing directory, where protocols already live at `svc/<name>`. A directory at `/svc`, under it or above it would meet the protocols' service nodes. Compared as a string, `/svc` would be a prefix of `/svcx`, which is another directory; the rule compares segments (`_is_prefix`), as the rule for a use's path does.
- Two directory declarations of one component may not overlap, neither path a prefix of the other by segments. A tree is served at each path, and an Open walks down from the outgoing root: with `/export/data` and `/export/data/x` declared, the Open for `export/data/x/f` would reach one tree or the other depending on which was added first. `/export/data` and `/export/data2` are two places, and two directories may share the directory above them (`/export/a`, `/export/b`).
- The rights are required: the walk's rule is that stated rights only grow toward the source, so a declaration that states none is an unbounded top and holds no hop to the ceiling the provider means. `route()` accepts such a record (it narrows nothing); that is why the requirement is the compiler's.

Why the walk only warns, and why it is the order it is. `route()` answers AccessDenied at the first hop that states fewer rights than the last hop that stated any. Walking from the use toward the source, rights may only grow, and a hop is held against the hop before it, not against the use: the use asks r*, the offer above it offers rx* from a hop that offered r*, and the use would be satisfied by either, but the offer promised more than it had. The Python walk is that rule written again (`_Hops.apply` is `detail::Hops::apply`), with the same lookups, in the same order, so that the first failure is the same hop: a route that is denied at an offer is denied there, whether or not the provider further up has a program. A failure that is not an AccessDenied (NotFound, a route that leads nowhere) is left alone, as the type check leaves a protocol route that does not resolve: the root answers it, and a deliberate failure must still build. The one place the two walks differ on purpose is the subdir: it is not joined, so a joined path that is too long (route()'s NotFound) is not seen, and a route that route() stops there before a hop that holds less would be warned for that hop.

The name `check_directory_rights` and not the design's `check_rights`: the module already has `check_rights(value, where)`, which parses a rights clause into a mask, and `component_builtins.py` calls it. A second function of that name would have replaced the parser, and every directory use would have failed with a `TypeError` (the first run of the tests showed it).

**Change**
The three refusals, before and after:

```python
# before                                                  # after
if isinstance(clause, dict) and "directory" in clause:    clause = _check_keys(clause, ("protocol", "directory",
    raise ManifestError("... cannot declare a directory     "type", "path", "rights"), at)
    ... (D3)")                                            kind, names = _capability_names(clause, at)
clause = _check_keys(clause, ("protocol", "type"), at)    if kind == KIND_PROTOCOL: ... refuse `path`, `rights`
...                                                       else: path and rights required, no `type`, no `subdir`,
allowed = ("parent", "self", "#child")                    one name, the path rules, the overlap rule
if kind == KIND_DIRECTORY: allowed = ("parent",)          source = _source_ref(clause["from"], ("parent", "self", "#child"), ...)
```

The keys, and what each refusal says (every message names its key):

| Clause | Refused | Message begins |
|---|---|---|
| directory declaration | no `path` | `capabilities[i]: a directory declaration needs `path`` |
| directory declaration | no `rights` | `capabilities[i]: a directory declaration needs `rights`` |
| any declaration | `subdir` | `capabilities[i].subdir: a declaration has no `subdir`` |
| directory declaration | `type` | `capabilities[i].type: a directory has no `type`` |
| protocol declaration | `path`, `rights` | `capabilities[i].path: a protocol declaration has no `path`` |
| directory declaration | a list of names | `capabilities[i].directory: a declaration names one directory` |
| directory declaration | a path that is not absolute, is `/`, ends in `/`, or has an empty, `.` or `..` segment | `capabilities[i].path: '...' ...` (`check_path`'s) |
| directory declaration | `/svc`, under it | `capabilities[i].path: '/svc/x' is /svc or lies under or above it` |
| directory declaration | overlapping another | `capabilities[i].path: '...' overlaps '...' (capabilities[j].path)` |
| protocol expose | `rights`, `subdir` | `expose[i].rights: only a directory expose has `rights`` |
| expose, offer of a directory | `type` | `expose[i].type: a directory expose has no `type`; only a protocol has one` |

An expose of a directory takes `from = self` (a declared directory: the check that compares kinds already existed) or `from = #child`, with `rights` and `subdir` read by the rules an offer's follow. The cycle check was already blind to the kind, so an offer of a directory from a child makes the same edge a protocol's does; a test now says so, because a rule that covers a case by accident is one a later edit breaks.

The bytes: `encode()` already wrote the fields at the offsets step 1 fixed, so the parser only had to fill them. `test_a_compiled_directory_writes_its_path_rights_subdir_and_flags` reads a compiled manifest back at the offsets `format.hpp` gives, without the structs that wrote it: the path in a Capability's record at 24, its rights and the `RightsStated` flag, an Expose's subdir at 40 and rights at 48, and the same fields empty and the flag off in a protocol's records.

The walk, in the shape of `route()`: the use is the first hop, then offers up through the parents, exposes down through the children, and last the declaration or the root's built-in. Each hop is a `RightsHop` (the word of its line, what the manifest says, the rights it states, and where `route()` would report it: instance, verb, name), and `_Hops.apply()` holds it against the last hop that stated rights: `hop.rights & held == held`. A route that is denied comes back with the hop that holds less and the one it was held against, from which the warning is made: the fix names the rights the hop before it stated and says which hop to ask for fewer.

The warning goes to stderr before any type error, because both are facts about the same tree and a build that fails should say everything it knows; it does not change the exit status. `--no-type-check` (`type_check = false`) turns both walks off, because the tree is not read at all. A route that goes above the top of the tree is walked only when a built-ins file is given, as for protocols; the denied offer of `/denied` is in the tree and warns with or without one.

The design's example prints the rights column unaligned; the text here pads each hop to the longest and puts the rights in one column, which is the only difference from the example.

Fixtures: `shelf.cm` offers `store`'s directory `data` (declared `rw*` at `/export/data`, exposed three ways) to `reader.cm` eight ways, and `route_test.cpp` and the Python walk are checked against the same table of answers: the route that works (`shared/public/docs`, the declared path carried apart from the subdir, the use's rights and not the declaration's), an expose that states nothing, an offer that asks more than the expose states (AccessDenied at store, Expose), an expose and an offer that ask more than the declaration allows (AccessDenied at store, Declare), a directory use of a protocol's name and a protocol use of a directory's (NotFound at store, Expose) and a provider with no program (NotFound at bare, Declare). Three of the uses are denied on purpose, so `shelf.cm` builds with three warnings each time it is compiled. The patched-bytes tests kept in `route_test.cpp` are those that need a record the compiler never writes (a declaration that states no rights, a protocol that states a path, a path of more than 1024 bytes) or that vary one hop at a time over the six-record route of `top.cm`; seven that covered the same ground as the compiled ones were removed.

**Reference**
The design follows Fuchsia's component manifest, as decisions 2 to 4 of `.agents/tasks/directory-capability/DESIGN.md` record, and the cites below are the design's own, not re-read in this step: a directory declaration states a path (`sdk/fidl/fuchsia.component.decl/capability.fidl:64-76`) and rights, and the validator refuses one without rights (`src/sys/lib/cm_fidl_validator/src/lib.rs:1379-1406`); an expose of a directory may state rights and a subdir (`sdk/fidl/fuchsia.component.decl/expose.fidl:130-136`); the request is held against the rights of the capability (`src/sys/lib/routing/src/rights.rs:22-33`, `:108`). The warning has no counterpart there: Fuchsia's `cmc` refuses a manifest whose routes it can see are wrong, and decision 9 (c) chose to warn instead, so that a deliberate failure builds.

**Verification**
- `python3 system/test_component_manifest.py` with `OURS_BUILD_DIR=out/dc-step2`: 152 tests (102 before; seven replaced or rewritten, fifty-odd added), all pass, `FixtureRoutesTest` among them (it ran, it did not skip).
- Host tests of the library: `decl` 26 to 27 tests, `route` 63 to 65 (seven patched-bytes tests removed, nine over the compiled fixtures added), all pass.
- Mutation checks, each applied to the file, run and reverted (the scripts are in the session scratchpad, not in the tree): 43 on `component_manifest.py` (21 on the parser, 16 on the walk and the warning, 6 on `main()`) and 15 on `route.hpp`, `route.cpp`, the fixtures and the compiler's output as the C++ tests see it. Every one is caught. Two survived the first run and were closed: an expose from the parent followed by the walk (the hand-built instances could not reach a resolution through it; the test now gives the root a declaration to find) and a walk run over protocol uses too (no protocol hop states rights, so the output could not differ; a spy now sees which uses are walked). The ones worth naming: the `/svc` rule as a string prefix (caught only by `test_a_declared_path_is_not_svc_or_under_it`, through `/svcx`), the overlap rule as a string prefix (caught only by `test_declared_paths_do_not_overlap`, through `/export/data2`), every hop held against the use and not the last hop (caught by seven tests, the one that matters being `test_a_hop_is_held_against_the_hop_before_it_not_the_use`), the expose not a hop (C1: seven patched tests and the compiled `RouteShelf` ones), the declaration not a hop (C2), the expose's subdir not joined (C4, caught by the compiled `ADirectoryUseEndsAtTheProviderWithTheDeclaredPath`, which replaced a patched test), and a provider without a program routed (C6, `RouteShelf.AProviderWithoutAProgramIsNotFound`).
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step2`) on `decl_test.cpp` and `route_test.cpp`: no diagnostic on any line of `system/lib/component`; what remains is googletest's header. `format_cpp.py --check` and `format_gn.py --check` are clean on every file changed.
- The six `.cm` files of the dev image compile to the same bytes as with the previous commit's compiler. The five fixtures with a directory in them are refused by it, as they should be.
- `boot_check.py --out out/dc-step2`: see `.agents/tasks/directory-capability-step2/RESULT.md` for the counts, which are those of the commit before.

**Deliberately not done**
- A marker on a use or an offer that silences the warning of a failure that is meant. The design leaves it open; until it exists `root.cm` warns each time its action runs, once now and three times when the first provider lands, and `shelf.cm` warns three times.
- The joined subdir is not walked, so route()'s NotFound for a path too long is not reported. The compiler refuses any one subdir over 1024 bytes; the sum of several is a case nobody writes.
- A check that a declared directory has a program. A component with no program may declare a directory, as it may declare a protocol (`hollow.cm`); the router answers NotFound, and that is a route that does not resolve, which the build leaves to the run time.
- A real provider, `examples/files` and any change to the dev image's manifests: steps 3 and 4.
- The guide: its passages that now describe the language wrongly are listed in `.agents/tasks/directory-capability-step2/RESULT.md`.

**How it was found**
None. A new feature, from the design. The one thing the first run turned up is the name clash above.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A use asks r*, the offer above it offers rx*, and the offer above that offered only r*. Which hop does the warning name, and why is it not the use or the hop that offered rx*?
2. Why does the walk stop at the first hop that holds less instead of listing every hop that does, and why does it say nothing for a route that ends in a provider with no program?
3. Why does the rule for two declared paths compare segments, and what would an Open for `export/data/x/f` reach if `/export/data` and `/export/data/x` could both be declared?
