# protocol-identity-root: the root's built-ins are one generated list, and its log names a protocol mismatch

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 4 of 5 of `.agents/tasks/protocol-identity/PLAN.md`, the last implementation step. The root's built-in capabilities become one data file that both the manifest compiler and `realm.cpp` read, so that the build check also resolves a route that ends at a built-in; `log_route_error()` names both protocols of a mismatch; and the experiment of 2026-10-29 is run again to show the whole chain. The scope ends at the root and the image's manifests: the documents of step 5, and the id and `switch` of `docs/repairs/261028/init.md` item 1, are not part of this step.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| How `realm.cpp` takes the generated list | (a) `#include "builtins.inc"` inside the anonymous namespace, where the hand-written list was; (b) a generated header with the list in its own namespace | (a): the brief says the same place and the same namespace, and the include file `component_builtins.py` writes has no namespace of its own | Not asked | (a), by the brief |
| What ties a library's name to the list | (a) a `constexpr` lookup in `BUILTINS` and one `static_assert` per name; (b) a `static_assert` on the count and the order | (a): the order of the JSON is free, and a name is what `connect()` compares | Not asked | (a), by the brief |
| Where the scratch experiment starts | (a) a worktree started after this step's commit, as the brief says; (b) a worktree started from the commit before, with this step's diff applied | (b): the change record holds the experiment's output and is part of the one commit, so the commit cannot come first without an amend; the diff is the commit's code | Not asked | (b), by this step; reported to the orchestrator |

## Steps
1. `system/service/init/builtins.json`: the seven built-ins in the order of the old `BUILTINS`; each name checked against the constant the old list used.
2. `system/service/init/BUILD.gn`: the action `:builtins.gen` (script `//system/component_builtins.py`, source `builtins.json`, input `component_manifest.py` because the script imports it, output `builtins.inc` under `$target_gen_dir`), the config that adds `$target_gen_dir` to the include path, and the group `:builtins` that carries both to `:init`, as `:programs` does.
3. `realm.cpp`: `#include "builtins.inc"` where the list was, with the comment updated; `has_protocol_builtin()` and six `static_assert`s; `log_route_error()` with a line of its own for `BadType`.
4. `kernel/products/image/dev/BUILD.gn`: `builtins = "//system/service/init/builtins.json"` on `root.cm`.
5. Build and run the tests; edit-and-revert a seventh `static_assert` to see the build refuse it.
6. The scratch experiment, in `.worktrees/pi-root-experiment`: (a) the Launcher offered to `#pc-ps2` as `ours.resource.Irq`, check on; (b) the same with `type_check = false` on `root.cm`, booted; (c) the provider-side line, by `routing_checks`' `type` changed to a wrong value. Delete the worktree and its branch.
7. Documents: `docs/changes/261029/init.md`, `docs/repairs/261028/init.md` item 1 rewritten, `docs/repairs/261029/ours-verify.md` for a hazard found; `scripts/lint_docs.py --changed` on each.
8. `ours-verify` on both images, then one commit: `feat(init): the root reports a protocol mismatch, and its built-ins are one generated list`.

## Files
- `system/service/init/builtins.json` — new
- `system/service/init/BUILD.gn`, `system/service/init/realm.cpp` — the action, the include, the `static_assert`s, the log line
- `kernel/products/image/dev/BUILD.gn` — `builtins` on `root.cm`
- `docs/changes/261029/init.md` — new
- `docs/repairs/261028/init.md` — item 1 rewritten; `docs/repairs/261029/ours-verify.md` — new
- `.agents/tasks/protocol-identity-root/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `python3 system/test_component_manifest.py` (with `OURS_BUILD_DIR` set to this worktree's `out/bringup.x86`) and `python3 system/test_component_builtins.py` end with `OK`; the `decl` and `route` host tests pass.
- clang-tidy on `realm.cpp` against `out/bringup.x86` reports nothing.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py`: both images build and boot, `routing_checks: passed` on rel, host tests pass; the real `root.cm`, now compiled with `--builtins`, is not rejected.
- `grep -n "component::Builtin{" system/service/init/realm.cpp` finds nothing; a `static_assert` on a name that is not in the JSON fails the build.
- The experiment: (a) fails the build with the text of `DESIGN.md`, "Error output", item 1; (b) logs `init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)` and pc-ps2 reports `getting the IRQ resource: BadType`.
