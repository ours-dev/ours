# protocol-identity-runtime: route() stops at a capability of another protocol type

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 2 of 5 of `.agents/tasks/protocol-identity/PLAN.md`: when the walk of `route()` ends, compare the use's `type` with the type of the end, and answer `BadType` with both types on a mismatch. The scope ends at the library: `route.hpp`, `route.cpp`, the router's test and its fixtures. The build-time check is step 3, in parallel, on disjoint files; the root's log line and `realm.cpp` are step 4.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| The shape of the error | (a) `RouteError` gains `want` and `got`, views, defaulted; (b) the root formats the text and `route()` returns it | (a): the root has no heap, and the log line formats where it prints | Not asked | (a), by the plan (`.agents/tasks/protocol-identity/PLAN.md`, "The run-time error") |
| Where the types are compared | (a) once, where the walk ends; (b) at each hop | (a): an offer and an expose carry no type, so a comparison at a hop would run on the end's type, found early | Not asked | (a), by the design |
| The fixture that mismatches | (a) one new root, `mismatch.cm`, over the existing provider.cm and consumer.cm; (b) a new provider and consumer pair | (a): the smallest tree, and no new program or manifest besides the root | Not asked | (a), within the plan's "keep it as small as you can" |
| Existing fixtures that rename a protocol without stating its type | (a) state the type on the use, in a `use` entry of its own; (b) leave them and let the tests expect BadType | (a): they are routes that must work, and step 3's build check would refuse the trees otherwise | Not asked | (a), by this step |

## Steps
1. `route.hpp`: `RouteError::want` and `got`, `detail::builtin_type()`, the two comparisons in `route()`, and the spec comment (the type check, the BadType error, the invariants and the exclusion). `route.cpp`: `builtin_type()`.
2. Fixtures: `mismatch.cm` with `type_check = false` and a comment on why, embedded as `MISMATCH`; `user.cm` and `leaf.cm` state the type of the renamed protocols they use.
3. `route_test.cpp`: the third tree in `Tree`, the BadType line in `describe()`, helpers that set a use's or a capability's type and give the Launcher a type and rights, and nine tests in `RouteTypes`.
4. Verify as the Test plan says, then one commit: `feat(component): route() stops at a capability of another protocol type`.

## Files
- `system/lib/component/include/component/route.hpp`, `system/lib/component/src/route.cpp` — the comparison, the error and the spec
- `system/lib/component/test/route_test.cpp`, `system/lib/component/test/BUILD.gn`, `system/lib/component/test/fixtures/BUILD.gn` — the tests and fixtures
- `system/lib/component/README.md` — the router test routes three trees
- `docs/changes/261029/component.md` — the change record
- `.agents/tasks/protocol-identity-runtime/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then both run from `out/bringup.x86`: every test passes; `route` has nine more than before.
- Every use of `route.hpp` builds, the init service included (the full build in `boot_check.py`).
- clang-tidy on `route.cpp` and `route_test.cpp` reports nothing in the component library; clang-format and gn format are clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py`: both images build and boot, `routing_checks: passed` on rel, and the host tests pass. The image runs the comparison on every Open, so a boot is the proof that no route of the image is a mismatch.
