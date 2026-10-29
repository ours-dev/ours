# protocol-identity: a route that ends at another protocol is a build error, and at run time a logged BadType

**Date**: 2026-10-29

## Delivered
A protocol capability, a protocol use and each of the root's protocol built-ins carry a `type` (the entry's own name when left out), and a route must end at a capability of the type its use states. The check is made twice: when the image is built, and on every Open. The design is `docs/design/261029/protocol-identity.md`; the plan is `PLAN.md` here; each step has its own workspace `.agents/tasks/protocol-identity-<step>/` with `PLAN.md` and `RESULT.md`.

- Design and plan: `cc7e6a84` (proposed), `b2a771a8` (decisions recorded, plan), `585b8b46` (plan updated).
- Step 1, `ca2d7a88`: manifest format version 2. A Capability and a Use carry a `type` string; the manifest language has the `type` key; `component_manifest()` takes `type_check`.
- `aa4018f5`: routing_checks' renamed use of `ours.test.Echo` states `type = "ours.examples.echo.Echo"`. Made ahead of steps 2 and 3, because both boot the image.
- Step 2, `ce7385e3`: `route()` compares the types where the walk ends and answers `BadType`, with `want` and `got` in `RouteError`.
- Step 3, `dc647693`, and `35b46992`: the manifest compiler walks the whole tree of a manifest that has children and fails the build, with a message naming the use, both types and every hop, when a route that resolves ends at another type. `system/component_builtins.py` reads the root's built-ins.
- Step 4, `5fd3002e`: `system/service/init/builtins.json` is the one list of the root's built-ins, from which `BUILTINS` in `realm.cpp` is generated and which `root.cm` hands the compiler; the root's log has its own line for `BadType`.
- Step 5 (this commit): the design moved to `docs/design/261029/protocol-identity.md`, the library's README, the glossary and the first component-framework design updated.
- `e0c20b99`: the repair item for the image packaging that reruns on every ninja run.

## Verification
- The experiment of 2026-10-29, run again in a scratch worktree (step 4; the outputs are in `docs/changes/261029/init.md`). With the check on, the build fails with the message of the design's "Error output" item 1. With `type_check = false` the boot log shows `init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)` and `pc-ps2: getting the IRQ resource: BadType`, where the first experiment showed `PeerClosed` and no line from the root.
- `python3 system/test_component_manifest.py`: 99 OK with the fixtures' build directory (`OURS_BUILD_DIR`), 94 with one skipped otherwise. `python3 system/test_component_builtins.py`: 26 OK.
- Host tests `decl` 20 passed and `route` 41 passed; clang-tidy on `decl.cpp`, `route.cpp` and `realm.cpp` clean.
- `boot_check.py` after step 4: dbg 44 passed, 0 failed; rel 41 passed, 0 failed, 6 of 6 required lines including `routing_checks: passed`; 148 host tests passed. The integration head differs from the tree step 4 verified only by `docs/repairs/261029/build.md`, so that run stands for it. This step changed documents and a path in one code comment, and was not booted again.
- Mutation checks in steps 2 and 4: removing the protocol-only guard, comparing before a built-in's rights hop, and a `static_assert` for a name not in `builtins.json` each fail the tests or the build.

## Deviations from the design
- The decoded `type` is a field filled with the name when the stored string is empty, not an accessor `type()`; a directory use decodes with a type equal to its name as well.
- `Builtin::type` is the last member of `Builtin`, so a designated initialiser writes `.rights` before `.type`.
- A `type` with a list is refused even for a list of one name.
- The `type_check` parameter exists for fixtures that exercise the run-time check on purpose (`mismatch.cm`); the design did not name it.
- The build error has no closing `capability` or `built-in` line and starts with `error:` and not with the compiler's usual prefix: the design's example is the text that was pinned.
- `component_builtins.py` refuses a duplicate name whatever the kind; `route()` tells a directory and a protocol of one name apart.
- Decision 3 (a) was taken at its smaller size: `BUILTINS` is generated, but `Realm::connect()` still chooses the server by comparing names, and the `static_assert`s tie the names the libraries define to the list in one direction only.
- The routing_checks manifest change was made first, as a commit of its own, where the plan had it in step 4.

## Found along the way
- A second ninja run redoes the image packaging because the `*_dist` actions' depfiles are missing; reproduced without any change of this task. `docs/repairs/261029/build.md`.
- `boot_check.py` and `kshell_run.py` keep their logs in a directory named after the out directory's last component only, so two worktrees on the default `out/bringup.x86` share it. `docs/repairs/261029/ours-verify.md`.
- The fixtures cross-check failed on a build directory older than a fixture, instead of skipping as it does with no build directory; fixed in `35b46992`.
- `user.cm` and `leaf.cm` renamed protocols with no type and resolved only because nothing compared types; they state one now.

## Not done
- The id and the `switch` in `Realm::connect()`: `docs/repairs/261028/init.md` item 1, rewritten for what remains.
- An on-target case in routing_checks that shows `BadType`: the build check refuses the image such a case needs. The evidence is the scratch experiment above, and `route()`'s host tests stand for the run-time half.
- The root's run-time log line carries the end of the walk but not its hops; the build error has them.
- A cross-check of the Python walk against `route()` itself: it compares with a table copied by hand from `route_test.cpp`.
- A generator for the protocols' codecs, and a type derived from one (non-goals of the design).
