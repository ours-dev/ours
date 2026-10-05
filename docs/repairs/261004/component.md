# component outstanding repairs

## 1. Every route copies a 1 KiB Subdir at least twice

**Severity**
Low: a performance observation, not measured. Nothing is wrong, and no route runs on the target yet.

**Symptom**
Latent, cannot be observed today: nothing calls `route()` until the root does in step 13. Once it does, each route returns a `Route` that holds a `Subdir`, `MAX_PATH_LENGTH` (1024) bytes inline, by value. Building the result copies it into `ustl::ok()`'s `OkValue` and from there into the `ustl::Result`, and the caller copies it again if it keeps the Route. Under D11 every Open of `/svc/<name>` routes again, so a busy client pays a few KiB of copying per Open, on the one thread every request waits behind (D16).

**Cause**
`Subdir` (`system/lib/component/include/component/route.hpp`) keeps the joined subdirectory inline because user programs have no heap (`system/lib/ort/src/cpp_dependencies.cpp:10-26`), and `route()` returns it inside its result so that the function stays pure. `std::array<char, 1024>` has no cheaper move than a copy. A protocol route, the common case on `/svc`, never has a subdir at all, yet carries the full buffer.

**Change**
Measure first, as CLAUDE.md asks of any performance work: a benchmark in `benches/` that routes a `/svc` protocol use through a tree the size of dev's root manifest, timing `route()` alone and an Open end to end, so the copies can be weighed against the channel round trip they sit inside. Candidates, to put to the user with the numbers:
- the caller passes a `Subdir &` to fill, one per root since one thread routes at a time (D16): no copy, one more parameter;
- the Route holds the hops' subdirs as views into the manifests, at most one per level, and the caller joins them once into the path buffer it builds the Open from;
- keep it, if the copies are noise next to the channel write.

**Reference**
Fuchsia's `RelativePath` is a `FlyStr`, and `RelativePath::extend()` allocates a new string at every hop that has a subdir (`src/sys/lib/cm_types/src/lib.rs:928-942` at `51f05104c13`); its router returns the path inside `RouteSource::new_with_relative_path()` (`src/sys/lib/routing/src/lib.rs:978` at `9e015f591c4^`).

**Acceptance**
The benchmark's numbers are in the change record, the user has chosen, and `out/<dir>/host/obj/system/lib/component/test/route` still passes.

## 2. The child URL scheme `fuchsia-boot:` still carries another system's name, a question for the user

**Severity**
Low: naming. Nothing is wrong at run time; every writer and reader of the URL is built from this tree.

**Symptom**
Every child record a manifest compiles to holds `fuchsia-boot:///#meta/<name>.cm`, and the root parses that prefix back out (`system/service/init/realm.cpp:35`, `ROOT_URL` in `system/service/init/realm.hpp:27`). The spelling also appears in `system/component_manifest.py:58` (`BOOT_URL_PREFIX`), `system/component_manifest.gni:49`, the comment on `Child` in `system/lib/component/include/component/format.hpp:152`, and the tests (`system/lib/component/test/decl_test.cpp:104-106`, `route_test.cpp:56`, `system/test_component_manifest.py:119`, `:439`).

**Cause**
Design D26 chose option (a), Fuchsia's spelling, "as recommended" on 2026-10-04, for the same reason D20 first chose Fuchsia's protocol names: a reader could look the string up in Fuchsia. On the evening of 2026-10-04 the user revised D20 so that the protocol libraries take ours's own names (`ours.io`, `ours.process`, `ours.kernel`, which became `ours.resource`; `docs/changes/261004/ours-io.md`). That decision named the protocol libraries only, not the URL scheme, so the rename left it as it was. D26's reason for (a) is now the one the user rejected for D20, and D26 already listed (b), "a scheme of ours's own, such as `boot:meta/<name>.cm`", so the choice should be put back to the user rather than made here.

**Change**
For the user to decide, with D26's Decided cell updated to match:
- (a) keep `fuchsia-boot:///#meta/<name>.cm`;
- (b) a scheme of ours's own with the same shape, `ours-boot:///#meta/<name>.cm`: one constant in the compiler, one in the root, `ROOT_URL`, the .gni comment and the test strings; the compiled format does not change, since the URL is a string in the string table;
- (c) a shorter scheme of ours's own, D26's `boot:meta/<name>.cm`: the same constants change, and the URL loses the `#` that separates the package from the manifest's path in it, which matters only once there is more than one package.
Recommended: (b), the smallest change that follows the user's D20 decision. As in the protocol rename, every peer is built in one build, so nothing old has to keep parsing the former spelling.

**Reference**
None, this project's own naming. The original spelling is Fuchsia's boot resolver URL (`src/sys/component_manager/src/builtin/fuchsia_boot_resolver.rs`), as D26 records.

**Acceptance**
The user's choice is in D26's Decided cell. For (b) or (c): `git grep -n 'fuchsia-boot'` outside `docs/` finds nothing, `python3 system/test_component_manifest.py` and the `decl` and `route` host tests pass, and boot_check passes both images with `routing_checks: passed` on rel.
