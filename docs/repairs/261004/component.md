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
