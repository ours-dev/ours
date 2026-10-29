# protocol-identity-build: the build rejects a route that ends at another protocol type

**Date**: 2026-10-29

## Delivered
Step 3 of `.agents/tasks/protocol-identity/PLAN.md`, one commit on the branch `pi-build`, `feat(component_manifest): the build rejects a route that ends at another protocol type` (its hash is in the report that handed this step back, since a commit cannot name itself), on top of step 2 (`ce7385e3`):

- The compiler checks the whole tree of the manifest it compiles. Each instance of the tree (a manifest under two parents is two) has each of its protocol uses walked the way `route()` walks it: up through the parents' offers, down through the children's exposes, to a capability of an instance with a program, or, above the top of the tree when a `builtins` file is given, to a built-in of the same kind and name. A use whose type (its `type`, or its name) differs from the end's is a build error; a route that does not resolve, and every directory use, is left alone. All mismatches are printed, ordered by instance path and then by use, before the compiler exits non-zero.
- The error is the text of `DESIGN.md`, "Error output", generalised: one `use`, `offer` or `expose` line per hop, the end named in the sentence above them. The experiment of 2026-10-29 is pinned byte for byte, and so is a four-hop provider-side mismatch through two exposes.
- `component_manifest.gni`: each manifest group's metadata carries `component_manifest_tree` (label and `.source.json`), a `<name>.tree` file collects the descendants at any depth, the compile action takes `--tree`, writes a `depfile` listing every scope it read, and takes `builtins` as `--builtins` and as an input. `type_check = false` passes `--no-type-check` and the compiler then reads neither the tree nor the built-ins. The `type` paragraph and the `type_check` entry of the header, and the help of `--no-type-check`, no longer say that nothing checks.
- `system/component_builtins.py`: `load()` validates the built-ins' data file and returns the entries; the command line writes the C++ include file that defines `constexpr auto BUILTINS = std::array{component::Builtin{...}, ...};` in the order of the file. Its output was compiled as part of a scratch translation unit inside an anonymous namespace, with `static_assert`s on the kind, rights and type of its entries.
- Tests: `system/test_component_manifest.py` (99 tests, 63 before) and the new `system/test_component_builtins.py` (26).
- `docs/changes/261029/component-manifest.md`: a section appended.

## Verification
- `python3 system/test_component_manifest.py`: 99 tests OK. `python3 system/test_component_builtins.py`: 26 tests OK. Both under Python 3.9.6, standard library only.
- The cross-check (`FixtureRoutesTest`) reads the scopes `gn gen` wrote for the named fixtures `realm`, `provider`, `consumer`, `hollow`, `top`, `user`, `leaf` and `mismatch`, and compares the walk's answer for every protocol use of the trees `realm.cm` and `top.cm` with a table taken from `route_test.cpp`'s expectations: 18 protocol uses, 9 of them resolving (4 and 3 in `realm.cm`'s tree, 14 and 6 in `top.cm`'s). It skips with a message when no build directory has them. The same trees have no mismatch, with the built-ins of `route_test.cpp` and without; `mismatch.cm`, which step 2 builds with `type_check = false`, has exactly the two (the provider's Echo offered as the Launcher, the built-in Launcher offered as `echo`), and the second is seen only when the built-ins are given.
- Mutation check, by hand and not committed: dropping the has-program rule, the kind match in the offer lookup, the target match in the offer lookup, or the kind match among the built-ins each fails the tests. Dropping the expose-from-the-parent refusal and the kind match among capabilities each survived the compiled-manifest tests, since the compiler writes neither; a test that builds the instances by hand now pins both.
- `gn gen out/bringup.x86` works; the build of everything passes with the check on, so the real `root.cm` (children `routing_checks`, `echo`, `shell`, `pc-ps2`, no `builtins`) is not rejected. `//system/lib/component/test:manifests` builds, and `decl` (20 tests) and `route` (41 tests) pass from `out/bringup.x86`.
- Scratch check 1, reverted: routing_checks' `type = "ours.examples.echo.Echo"` changed to `"ours.examples.echo.EchoWrong"` fails the image build, at `root.cm.compile`:

```text
error: //kernel/products/image/dev:root.cm: #routing_checks uses protocol "ours.test.Echo" (type ours.examples.echo.EchoWrong),
       but the route ends at #echo's capability "ours.examples.echo.Echo" (type ours.examples.echo.Echo):
         use    #routing_checks "ours.test.Echo"
         offer  root.cm "ours.examples.echo.Echo" from #echo to #routing_checks as "ours.test.Echo"
         expose #echo "ours.examples.echo.Echo" from self to parent
       Offer a capability of type "ours.examples.echo.EchoWrong" under that name, or give the use the type it means.
```

- Scratch check 2, reverted: a harmless capability added to `echo.cm` reran `echo.cm.compile` and `root.cm.compile`. That alone does not prove the depfile, because ninja already reruns the root's compile when a child's compile output changes, through the dependency on the child's group. The depfile was shown separately: a depfile naming a file newer than the output makes ninja rerun the action (`recorded mtime of ... older than most recent input`), and the real depfile lists the four descendants' `.source.json`.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-build`, with the final tree: build ok in 0.4 s (the preceding builds had warmed it); dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines (`routing_checks: passed` among them), 0 error lines; host tests PASS, 148 passed, 0 failed.
- `scripts/format_gn.py` clean on `system/component_manifest.gni`; `scripts/lint_docs.py --changed` clean on each document of this step.

## Deviations from the design
- The route in the build error ends at the last `offer` or `expose` line; there is no closing `capability` or `built-in` line. The brief asks for one, and the design's example, which it also asks to pin byte for byte, has none: the sentence above the route already says where it ends and with what type, and the last hop line names the instance. The example won.
- The error starts with `error:` and not with the `component_manifest:` prefix every other message of the compiler carries, as the example does. The mismatch text is printed as it is, one message per mismatch.
- A duplicate in the built-ins' file is the same name whatever the kind, where route() tells a protocol and a directory of one name apart. The root finds a built-in by its name alone, so the file is stricter than route() needs. Step 4 may loosen it if it has a reason.
- `<name>.tree` walks with `walk_keys = ["component_manifest_tree_walk"]`, a list of the children's groups, and not with GN's default walk through every dependency, which would also visit each child's program and everything it depends on. The plan says "no barrier"; the metadata key has none, and the walk continues through all the manifests.
- `--tree` is required, and `--depfile` is passed only when the check is on.

## Found along the way
- Every `*_dist` action of the image (`ours_dist`, `bootfs_dist`, `cmdline_dist`, ...) reports `depfile ... is missing` under `ninja -d explain`, so a second `ninja` run does 36 steps instead of "no work to do". It is not caused by this change (the `.cm` compiles are clean on a second run) and was not investigated.
- The `.cm` compiles already depend on their children's compile outputs through the phony targets of the children's groups, so a descendant's change reran the root's compile before the depfile existed; the depfile makes the dependence on the declarations themselves explicit.
- `top.cm` would have failed this check as it stood: `user.cm`'s `ours.example.RealmEcho` and `leaf.cm`'s `echo` are provider's `ours.example.Echo` renamed, and stated no type. Step 2 (`ce7385e3`) gave them one, in a `use` clause of their own, and this branch was fast-forwarded onto it before its tests ran, so this step edits no fixture.
- No defect found that is fixed nowhere, so there is no `docs/repairs/261029/` file.

## Not done
- `builtins` on `root.cm`, the data file `system/service/init/builtins.json`, the GN action that runs `component_builtins.py` and `realm.cpp`'s use of its output: step 4. Until then the real image's routes that end at a built-in (the Launcher, the `ours.resource.*` protocols) are not checked at build time.
- `system/lib/component/README.md` and the design documents do not describe the build check yet: step 5.
- A cross-check that reads `route_test.cpp` itself. `FixtureRoutesTest` compares the walk with a table copied from that test's expectations by hand, so a change to `route()` that also edits the test is not caught until the table is edited too. Recorded in the change record, "Deliberately not done".
