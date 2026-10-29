# protocol-identity-runtime: route() answers BadType for a route that ends at another protocol type

**Date**: 2026-10-29

## Delivered
Step 2 of `.agents/tasks/protocol-identity/PLAN.md`, one commit on the branch `pi-runtime`, `feat(component): route() stops at a capability of another protocol type` (its hash is in the report that handed this step back, since a commit cannot name itself):

- `route()` compares the use's `type` with the type of the end of the walk, once, and only for `Kind::Protocol`: at a provider `Capability::type`, at a built-in `detail::builtin_type()` (its stated type, or its name). A mismatch is `BadType`, verb `Declare`, `at` the providing instance or nullptr for a built-in, `name` the capability's name at its source, `want` the use's type, `got` the end's. A built-in's hop is applied first, so a rights failure keeps AccessDenied. Nothing else about the answers changes.
- `RouteError` gains `std::string_view want = {}` and `got = {}`, set for BadType only; every existing construction compiles unchanged. The spec comment of `route()` has "The type check", the BadType error, the view and no-allocation invariants, and the exclusion of what a program serves.
- Fixtures: `mismatch.cm`, a root over `provider.cm` and `consumer.cm` with two wrong offers (provider's Echo as `ours.process.Launcher`; the built-in Launcher as `echo`), built with `type_check = false` and embedded as `MISMATCH`. `user.cm` and `leaf.cm` state `type = "ours.example.Echo"` on the renamed uses, each in a `use` entry of its own.
- Tests: nine in `RouteTypes` (renamed use with its type, the default is the name at both ends, provider mismatch, the error field by field, a built-in without a type compared by its name, a built-in with a type, a directory never compared at a built-in and at a provider, another failure keeping its status).
- `docs/changes/261029/component.md` has the change record; `system/lib/component/README.md` says the router test routes three trees.

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then each run from `out/bringup.x86`: `route` 41 tests passed (32 before), `decl` 20 tests passed.
- Mutations, each undone: the protocol-only guard removed at the provider's end fails `ADirectoryUseIsNeverComparedAtAProviderEither` alone; removed at both ends it also fails `ADirectoryUseIsNeverCompared` and three subdir tests; the built-in's comparison moved ahead of its hop fails `ARouteThatFailsForAnotherReasonKeepsItsStatus` alone; `user.cm` and `leaf.cm` without their `type` fail `Route.TwoLevelsUpAndTwoDown`.
- clang-tidy against `out/bringup.x86` on `route.cpp` and `route_test.cpp`: nothing outside `vendor/googletest`. `scripts/format_cpp.py` and `scripts/format_gn.py`: clean. `scripts/lint_docs.py --changed` on each document: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-runtime`: build ok in 6.3 s (init included); dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines (`routing_checks: passed` among them); host tests PASS, 148 passed, 0 failed. Neither log has an `init:` line for a failed route beyond routing_checks' two deliberate ones, so no route of the image is a false positive.

## Deviations from the design
- `mismatch.cm` has two offers, not one: the second (the built-in Launcher as `echo`) is the built-in mismatch from a fixture, so the built-in tests need a hand-made `Builtin` list only for a built-in that states a type. The plan's "a parent that offers a capability to a child under a name whose type differs" is the first offer.
- The README of the library is edited by one sentence. The plan leaves its description of the checks to step 5, and this step does not describe them there; it only keeps "two trees" from being false.

## Found along the way
- `user.cm` and `leaf.cm` had renamed uses with no `type`. They resolved only because nothing compared types; with the comparison in they answered BadType. They state their type now, which step 3's build check needs of them too. No other manifest of the image renames a protocol without a type: `routing_checks` was fixed ahead of this step.
- `init`'s `log_route_error()` prints a BadType as `routes nowhere: BadType at <where> (declaration of "<name>")`: accurate, and silent on the types. Step 4 owns the line.
- No defect found that is not fixed, so there is no `docs/repairs/261029/` file.

## Not done
- The build-time check (step 3), and `type_check = false` is inert until it exists.
- The root's log line and the generated built-ins list (step 4), and `realm.cpp` is untouched.
- The description of the checks in the library's README, the glossary and the design document (step 5).
