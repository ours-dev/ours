# protocol-identity-root: the root's built-ins are one generated list, and its log names a protocol mismatch

**Date**: 2026-10-29

## Delivered
Step 4 of `.agents/tasks/protocol-identity/PLAN.md`, one commit on the branch `pi-root`, `feat(init): the root reports a protocol mismatch, and its built-ins are one generated list` (its hash is in the report that handed this step back, since a commit cannot name itself), on top of steps 1 to 3 (`35b46992`):

- `system/service/init/builtins.json` lists the root's seven built-ins in the order the hand-written list had them. `:builtins.gen` in `system/service/init/BUILD.gn` runs `component_builtins.py` on it and writes `builtins.inc`; `realm.cpp` includes that where `BUILTINS` was, so no hand-written list is left (`grep -n "component::Builtin{" system/service/init/realm.cpp` finds nothing). `has_protocol_builtin()` and six `static_assert`s tie `ours_process::LAUNCHER_NAME` and the five `ours_resource` protocol names to the list.
- `log_route_error()` prints `init: <moniker>'s use of "<name>" reaches another protocol: it wants <want>, the route ends at <got> (<where>)` for `BadType`, `<where>` being `the built-ins` or `provided by <moniker>`; every other status keeps its line. No allocation. The epitaph needed no code: `connect()` already closes the channel with `routed.unwrap_err().status`.
- `root.cm` passes `builtins = "//system/service/init/builtins.json"`, so the build check also resolves routes that end at a built-in. The real image passes it with no change to any manifest: the one renamed use (`routing_checks`' Echo) states its type since `aa4018f5`.
- `docs/changes/261029/init.md` (with the experiment's outputs), `docs/repairs/261028/init.md` item 1 rewritten for what remains, `docs/repairs/261029/ours-verify.md` (new).

## Verification
- `python3 system/test_component_manifest.py`: 99 tests OK (with `OURS_BUILD_DIR` set, so the cross-check over the fixtures ran). `python3 system/test_component_builtins.py`: 26 tests OK. `decl`: 20 tests passed. `route`: 41 tests passed.
- clang-tidy against `out/bringup.x86` on `realm.cpp`: exit 0, no diagnostic. `format_cpp.py` and `format_gn.py`: clean. `lint_docs.py --changed` on each document: no violations.
- `static_assert` check, by edit and revert: a name that is not in the JSON fails the compile of `realm.cpp`.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-root` with the final tree: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required lines, `routing_checks: passed`, 0 error lines); host tests PASS (148 passed, 0 failed). The rel log has no `reaches another protocol` line.
- The experiment of 2026-10-29: (a) with the check on, the build of `root.cm` fails with the text of `DESIGN.md`, "Error output", item 1, byte for byte; (b) with `type_check = false` the image boots, the root logs the `reaches another protocol ... (the built-ins)` line and pc-ps2 reports `getting the IRQ resource: BadType`; (c) a wrong `type` on routing_checks' Echo logs the same line with `(provided by echo)`. All three outputs are in `docs/changes/261029/init.md`. The scratch worktree and its branch are deleted.

## Deviations from the design
- The scratch experiment's worktree was started from the commit before this step with this step's diff applied, not after the commit: the change record that holds its output is part of the commit. The code under test is the commit's code.
- A third experiment, (c), was added to show the other branch of the log line, which the brief's two experiments do not reach (a provider and not a built-in).
- The `static_assert`s look a name up in `BUILTINS` (one direction). Whether `connect()` has a branch for each listed name is not tied; that is `docs/repairs/261028/init.md` item 1, rewritten.

## Found along the way
- `boot_check.py` keeps its logs and boot directories in `$TMPDIR/ours-verify-<out name>`, so two worktrees that both use the default `out/bringup.x86` share them. Not hit (the runs were sequential); recorded in `docs/repairs/261029/ours-verify.md`. `kshell_run.py` has the same rule.
- No defect of steps 1 to 3 found: `component_builtins.py` was used as it stood, and its output compiled unchanged inside `realm.cpp`'s anonymous namespace.

## Not done
- The description of the checks, the type and the data file in the library's README, the glossary and the design document: step 5.
- The id and the `switch` in `Realm::connect()`: `docs/repairs/261028/init.md` item 1.
- The hops of the route in the root's log line; the build error has them, the run-time error carries only the end of the walk.
