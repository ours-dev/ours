# manifest-compiler changes

## The compiler writes the children table in dependency order (2026-11-03)

**Files**

- `system/component_manifest.py` — `_order_children()`, called from `compile_manifest()`: the children come back ordered by the offers between them, stable, ties by manifest order; the module docstring says so.
- `system/test_component_manifest.py` — four tests for the order; one existing test's byte-level ref indices updated to the new table order.
- `system/component_manifest.gni` — documents the order under the `children` parameter.
- `system/lib/component/test/decl_test.cpp` — the two tests that pin a fixture's children table order expect the dependency order now.
- `docs/design/261004/component-framework.md` — D30; the startup step says manifest order is dependency order.

**Symptom**

A child that uses a capability of a sibling listed after it started before its provider: the Open of `/svc/<name>` at start walked a route to a Resolved-but-not-Started sibling, answered NotFound, and nothing ever retried it. With eager-only start (D4) and no restart (D23) that failure was permanent for the boot.

**Cause**

The capability graph already carries the start-order information — an offer from #a to #b is exactly "b needs a serving before b starts" — and `_check_cycles()` read it, but only to refuse cycles. The root starts eager children in manifest order, so who was ready when was the order the author happened to list the children in. Fuchsia starts eager children concurrently with no ordering (`cm/model/component/mod.rs:1205-1231`) and gets away with it because a provider reached before it started is started lazily on the route; ours cut lazy start (D4) and never restarts (D23), so the same laxity would be permanent. The missing piece was not a new source of truth but using the one the format already had.

**Change**

`_order_children()` builds the predecessor sets from the offers between children (only `source = #child` makes an edge: the parent is resolved before any of its children starts, and a built-in is above the root) and repeatedly takes, among the children whose predecessors are placed, the one the manifest listed first — a stable topological order, so an unrelated edit does not shuffle the table. `encode()` recomputes every child ref's index from the reordered list, so no index is patched by hand, and the type check walks by name, so it is untouched. Cycle refusal stays where it was, with its error naming the cycle; the sort cannot reach a stall on an acyclic edge set.

The root's rule "start eager children in manifest order" is unchanged: the manifest order now is the dependency order.

Rejected: computing the order at resolve time in the root (D30's option b) — the root has no heap, would run the sort per resolve, and the compiler is the only writer of the file, so build time is where the work belongs.

**Reference**

None, this project's own design. Fuchsia has no counterpart: it orders nothing at start (`mod.rs:1205-1231`) and leans on lazy start. The shape — declare producers and consumers of named resources, topologically sort, refuse cycles — is EnTT's `entt::organizer`, applied to the offers table the manifests already carry.

**Verification**

- `python3 system/test_component_manifest.py`: 156 tests OK (four new).
- `boot_check.py --out out/component-compiler.x86`: build ok; dbg PASS (46 passed, 5 skipped), rel PASS (44 passed, 8 skipped, 7/7 required — `routing_checks: passed`, the component tree routed at boot, whose children order the change also reorders); host-tests 159 passed, 0 failed.
- `clang-format --check` clean on `decl_test.cpp`; `clang-tidy` reports no diagnostics for it.

**Deliberately not done**

- Offers are not reordered: only the children table is the start order, so the offer table keeps the manifest's order and the compiled tables change as little as the feature needs.
- No lazy start and no retry of a failed Open: a route that still fails (a use of a capability nobody offers) stays a boot-time log line, as D23 decided.
- The sort uses only the offers between children; uses that route to the parent's own capability or to a built-in make no edge, because the parent is resolved before any of its children starts and a built-in is above the root.

**How it was found**

None — this change came out of a design review of the startup rule against EnTT's organizer, not from a failure report.

**The user's prediction**

None.

**Three questions you should be able to answer**

- Why does an offer between children order them, but a use of the parent's own capability not?
- Why must the sort be stable (ties by manifest order) rather than any valid topological order?
- Why could the root not simply retry a failed Open later, which would make the ordering unnecessary?

## The compiler also writes the manifest's names header (2026-11-03)

**Files**

- `system/component_manifest.py` — `names_header()`, `_identifier()`, `_unique_folding()`, and the `--names` mode of `main()`: the header comes from the scope alone, the other inputs become optional.
- `system/component_manifest.gni` — `<name>.names.gen` action, `<name>.names.config`, and the `<name>.names` group; the top comment documents the header.
- `system/test_component_manifest.py` — `NamesTest`, nine tests.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the `/svc` constants come from `<component/names/routing_checks.hpp>` now.
- `system/service/init/test/routing_checks/BUILD.gn` — the program depends on `:routing_checks.cm.names`.
- `docs/design/261004/component-framework.md` — D31.

**Symptom**

The strings a program and its manifest share were written twice, by hand, in two languages: the manifest names the capability, the program spells `/svc/<name>` again to open it, and a directory's `path` is written in the manifest and again in the server code. The two copies had no link: a typo in either read as NotFound from the provider's own server, and route()'s spec says so outright ("a typo between the two reads as NotFound").

**Cause**

The manifest was treated as the build's input only, and the program as its own source of truth for paths. But the manifest already holds every path the program opens: each use's `target_path` and each declaration's `path` are checked, unique, and compiled. The duplicate was not a missing fact but an unshared one — the compiler had the strings and the program asked the author for them a second time.

**Change**

`--names` makes the compiler write `component/names/<stem>.hpp` from the scope alone — deliberately not a second output of the `.cm` compile, since that reads the program's install name and the program must depend on the header, which would close a cycle. The mapping is mechanical, so the manifest language grows no syntax: every non-alphanumeric folds to `_`, uppercased (`ours.process.Launcher` is `USE_SVC_OURS_PROCESS_LAUNCHER`); a use's constant is keyed by its path without the leading `/`, a declaration's by its name; a leading digit is escaped. Two names the fold brings together (`/NOTES` beside `/notes`) are refused at build time, since the header could not spell both. Constants are `inline constexpr std::string_view` in `component::names::<stem>`, one namespace per manifest, so two manifests' constants cannot collide. The rules every manifest keeps run on the scope first, so a refused manifest never gets a header.

routing_checks is the first consumer: its five `/svc` constants and the fshost `Admin` path come from the header. `UNDECLARED` stays a literal on purpose — a name the manifest does not use is the point of that check.

Rejected: short names in the manifest (`as = "echo"` for a constant) — a second naming system to keep unique, against a mapping that is total and free. Rejected: emitting the header from the `.cm` compile as a second `outputs` entry — right-looking but cycle-forming, as above.

**Reference**

None, this project's own design. Fuchsia's programs keep spelling the literals (its routing integration tests open `/svc/<name>` by hand); cmc compiles `.cml` to `.cm` and offers no header. The identity-in-one-place shape is EnTT's compile-time id naming (`entt::type_hash`), applied to manifest strings; the user picked the proposal and the mechanical mapping (2026-11-03).

**Verification**

- `python3 system/test_component_manifest.py`: 165 tests OK (nine new).
- `ninja user-x86/phony/.../routing_checks` builds: the header generates, the program's aliases compile against it; `clang-format --check` clean, `clang-tidy` no diagnostics.
- Full `boot_check` (both images, host tests) after this entry was written: see the commit.

**Deliberately not done**

- The directory-use constants (`USE_FILES` and friends) are emitted but routing_checks does not consume them yet: its `/files`-family names are also assertions about what the *sibling* files serves, and moving those is its own step.
- No `PATH_*` consumer: no program serves a directory capability on dev yet (the directory-capability task is open), so nothing includes them.
- The header does not carry capability *names* (the bare `ours.test.Echo`, only the `/svc/`-joined path): no program needs the bare name today.

**How it was found**

None — second of the two proposals the user picked from the EnTT review of 2026-11-03.

**The user's prediction**

None. The user decided the naming scheme (mechanical mapping, over short names in the manifest).

**Three questions you should be able to answer**

- Why must the names header be generated from the scope alone rather than as a second output of the `.cm` compile?
- Why is the fold-collision (`/NOTES` and `/notes`) refused instead of one shadowing the other?
- Why is `UNDECLARED` the one `/svc` path routing_checks still spells as a literal?
