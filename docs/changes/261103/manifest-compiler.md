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
