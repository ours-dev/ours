# protocol-identity-build: the build rejects a route that ends at another protocol type

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 3 of 5 of `.agents/tasks/protocol-identity/PLAN.md`: the manifest compiler walks the whole tree of a manifest that has children and refuses, with an error that says exactly what is wrong, a protocol route that resolves and ends at a capability of another type. A route that does not resolve is left to run time, as today, so the deliberate failures of routing_checks still build. The scope ends at the compiler, the template's plumbing and the reader of the built-ins' data file; the run-time check is step 2, and the data file itself, its GN action and `root.cm`'s `builtins` are step 4.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| The route lines of the build error | (a) the lines of the design's example, `use`, `offer` and `expose` only, with the end named in the sentence above them; (b) also a closing `capability` or `built-in` line | (a): the design's example, which a test pins byte for byte, has no closing line, and the sentence above already names the end and its type | Not asked | (a), by the brief's rule that the example wins |
| What a duplicate in the built-ins' file is | (a) the same name; (b) the same kind and name | (a): the root finds a built-in by its name alone (`connect()`), so a protocol and a directory of one name would be two answers to one question; route() tells kinds apart, which a looser file could allow later | Not asked | (a), by this step; step 4 may loosen it |
| How the tree walk is bounded | (a) `walk_keys` naming the children's groups; (b) GN's default walk through every dependency | (a): the default walk would also visit each child's program and everything the program depends on | Not asked | (a), by this step |
| `--tree` | (a) required; (b) optional | (a): the template always passes it, and an optional one lets a caller forget it and silently skip the walk | Not asked | (a), by this step |

## Steps
1. Read the rules of the walk: the comment on `route()` in `system/lib/component/include/component/route.hpp` and `src/route.cpp`, and the trees of `fixtures/BUILD.gn`.
2. `component_manifest.py`: split `compile_manifest()` into the checks of a target's scope and `_declare()`, so a descendant's scope is read by the same rules; add the instance tree, the walk (`route_protocol_use()`, with `_find_builtin`, `_find_offer`, `_find_expose` and `_find_capability` named for route()'s lookups, and a comment per rule naming it by route()'s spec), `check_types()`, `format_mismatch()`, `load_tree()` and the depfile writer; `--tree`, `--builtins`, `--depfile` and a `--no-type-check` that now does something.
3. `system/component_builtins.py`: `load()` validates the data file and returns the entries; the command line writes the C++ include file.
4. `component_manifest.gni`: the `component_manifest_tree` and `component_manifest_tree_walk` metadata, the `<name>.tree` file, `--tree`, the `depfile`, the `builtins` parameter as `--builtins` and an input; the three texts that said nothing checks yet are rewritten.
5. Tests: `system/test_component_manifest.py` and the new `system/test_component_builtins.py`.
6. Verify as the Test plan says, then one commit: `feat(component_manifest): the build rejects a route that ends at another protocol type`.

## Files
- `system/component_manifest.py`, `system/component_manifest.gni` — the compiler and the template
- `system/component_builtins.py` — the reader of the built-ins' data file, and the generator of the C++ list
- `system/test_component_manifest.py`, `system/test_component_builtins.py` — their tests
- `docs/changes/261029/component-manifest.md` — the change record, a section appended
- `.agents/tasks/protocol-identity-build/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `python3 system/test_component_manifest.py` and `python3 system/test_component_builtins.py` end with `OK`.
- `gn gen` works, `//system/lib/component/test:manifests` builds with the check on, and the component host tests `decl` and `route` pass.
- A scratch edit, never committed: routing_checks' `type = "ours.examples.echo.Echo"` changed to a wrong value fails the image build with a message that names routing_checks and echo; a harmless capability added to echo.cm reruns the root's compile.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py`: both images build and boot, `routing_checks: passed` on rel, and the host tests pass. The real root.cm has children, so the check runs over the real tree on every build, without `builtins` until step 4: it must not reject the image.
