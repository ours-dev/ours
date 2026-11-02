# directory-capability-step2: the manifest compiler writes directories, and the build warns for a directory route that holds less

**Date**: 2026-10-30

## Delivered
Branch `dc-step2`, worktree `.worktrees/dc-step2`, one commit on `754ab32a`: `feat(component_manifest): a component declares, exposes and offers a directory`. It is step 2 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md`:

- `component_manifest.py`: `capabilities` takes `{ directory, path, rights }` (both required for a directory, refused for a protocol, `subdir` and `type` refused on either), with the path rules (absolute below `/`, clean segments, not `/svc` or under it by segments, no two directories of one component equal or one under the other); `expose` takes `directory` from `self` or a child with optional `rights` and `subdir` (refused on a protocol expose; `type` refused on any); `offer` of a directory comes from `parent`, `self` or `#child`. The three refusals and every text that named D3 and the root's built-in `boot` as the only directory are gone; the cycle check covers a directory offer from a child (it was blind to the kind already, and a test says so now). `encode()` needed nothing: it wrote the fields at the offsets step 1 fixed.
- The build-time rights walk (decision 9 (c)): `route_directory_use()` is `route()`'s directory walk in Python (the use, offers up, exposes down, the declaration, or the built-in `boot` when `--builtins` is given), `check_directory_rights()` runs it over every directory use of the tree, `format_rights_warning()` prints one `warning:` block per denied use to stderr, before any type error. The exit status stays 0 and the file is written. `--no-type-check` (`type_check = false`) turns it off with the type check, because the tree is not read.
- `component_manifest.gni`: the header documents the language for directories, the warning, and that `type_check = false` silences both walks.
- Fixtures: `shelf.cm` (root), `store.cm` (provider), `reader.cm` (consumer, eight uses) and `bare.cm` (a provider with no program) in `system/lib/component/test/fixtures/BUILD.gn`, embedded by the test `BUILD.gn`. `shelf.cm` builds with three warnings, on purpose.
- Tests: `system/test_component_manifest.py`, `route_test.cpp` (`RouteShelf`), `decl_test.cpp`.
- Documents: `docs/changes/261030/component-manifest.md` (an entry appended), this note.

The dev image does not change. `root.cm` and every real manifest stay as they are, and the six the image packages compile to the same bytes as with the previous commit's compiler. The only visible change is one warning each time `root.cm`'s action runs:

```text
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
           use    #routing_checks "denied"                                    rx*
           offer  root.cm "boot" from parent to #routing_checks as "denied"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.
```

## Verification
- `boot_check.py --out out/dc-step2` before (`754ab32a`) and after, the same counts:

  | | Before | After |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines | the same |
  | host tests | PASS, 151 passed (executables), 0 failed | the same |

- `OURS_BUILD_DIR=out/dc-step2 python3 system/test_component_manifest.py`: 102 tests before, 152 after, all pass; `FixtureRoutesTest` ran (11 tests, none skipped), and compares the Python walk with `route_test.cpp`'s answers on `realm`, `top` and `shelf`, for the protocol uses as before and now for the directory uses.
- Host tests of the library: `decl` 26 to 27 tests, `route` 63 to 65, all pass.
- Mutation checks, each applied to a file, run and reverted (scripts in the session scratchpad, not in the tree): 43 on `component_manifest.py` and 15 on `route.hpp`, `route.cpp`, the fixtures and the compiler's output as the C++ tests see it. Every one is caught. Two of the 43 survived the first run and were closed by a stronger test; they are named in the change record. The 15 include the rules the compiled fixtures now carry in place of patched bytes: the expose not a hop (C1), the declaration not a hop (C2), the declared path not carried (C3), the expose's subdir not joined (C4), the kind ignored in the expose lookup (C5, C10) and a provider without a program routed (C6). The fixture mutations (F1 to F3) show that the tests read the fixture, not a copy of its numbers.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step2`) on `decl_test.cpp` and `route_test.cpp`: no diagnostic on any line of `system/lib/component`; what remains is googletest's header. `format_cpp.py --check` and `format_gn.py --check` are clean on all five files. `lint_docs.py --changed` is clean on the change record and this note.
- The six `.cm` files of the dev image (`root`, `echo`, `hello`, `shell`, `pc-ps2`, `routing_checks`) were compiled by the previous commit's compiler and by this one from the same inputs: identical bytes. The three fixture manifests that hold a directory (`bare`, `store`, `shelf`) are refused by the old compiler.

## Deviations from the design
- **`check_rights()` is `check_directory_rights()`.** The module already has `check_rights(value, where)`, the parser of a rights clause, and `component_builtins.py` calls it. Defining the design's name replaced the parser and failed 20 tests at once (a `TypeError`). The walk, its twin of `route_protocol_use` (`route_directory_use`) and its formatter are named as the design says.
- **The warning's rights column is aligned.** The design's example has the use's `rx*` and the offer's `r*` in different columns; the code pads each hop to the longest one and prints the rights in one column. A hop that states no rights prints `none`. The fix sentence is made for the hop that holds less: `Offer`, `Expose` or `Declare rights that contain <the rights of the hop before it>, or ask the <use, offer or expose> for fewer`; for the built-in, `The root's built-in allows only <rights>; ask the <hop> for no more than that`. The text of the design's example is the case of an offer held against the use.
- **`root.cm` warns once, not three times.** The design says three: `/denied` and the two negative entries of the Testing section. Those two arrive with `examples/files` in step 4; today there is one.
- **A directory offer's and a directory expose's `type` refusal says so.** The shared text told the author to state the type where the capability is declared and used, which a directory has none to state. A directory clause now gets "a directory offer/expose has no `type`; only a protocol has one", and the existing test for offers was split in two to say it.
- **The `Tree` hook of `route_test.cpp` stays.** The step 1 note expected it to go once compiled fixtures replaced the patched tests; the remaining patched tests (`Chain` over `top.cm`) patch `top.cm`'s offer and need it.
- **"Neither path a prefix of the other, no path may lie above `/svc`".** The only path above `/svc` is `/`, which `check_path` refuses already; the rule is written for it all the same (`_is_prefix` both ways) and tested for `/svc` and under it.
- **The design's line cites** (`component_manifest.py:466`, `:649`, `:476-478`, `:488-492`, `:665-671`, the `gni` and `route.hpp` sentences) were right at `754ab32a`.

## Found along the way
- A route that is denied at an offer is denied there whether or not the provider above it has a program, because the first failure on the walk is the answer. A first draft of the tests expected the opposite for routes "that do not resolve" and failed on it; the test now asks each route to stop before any hop that would deny, and `test_the_first_failure_on_the_walk_is_the_answer` pins the other case. The same holds for `route()`: a user of this walk should read "a route that does not resolve" as "NotFound before any AccessDenied".
- A protocol use can never be denied by the walk, because no protocol hop states rights, so a walk run over protocol uses by mistake would print nothing. The filter is seen only by a spy in a test (`test_the_walk_is_run_for_directory_uses_only`), which is what closed that surviving mutant.
- `top.cm`, which has no built-ins file, warns twice when built (`narrow` and `wide`); with `route_test.cpp`'s built-ins it would warn a third time (`writable`, held against the built-in `boot`'s rx*). `test_without_the_builtins_a_denial_at_them_is_not_seen` pins that, so nobody takes the quiet build for a clean one.
- No repair item written.

## Not done
- A marker that silences the warning of a failure that is meant; the design leaves it open.
- Steps 3 to 9: `Outgoing::add_directory`, the root's `forward_directory()`, `examples/files`, and the rest. A directory that ends at a provider is still `NotSupported` at the root, and `Outgoing` still builds `svc/` and nothing else. The root also still has to answer InvArgs for a declared path that is empty or not absolute (the decoder does not check it, and `route()` only checks the length).
- The guide, `docs/architecture` and the design notes: not edited (the brief). The passages that now describe the language wrongly are below, for the later sync.

### Guide passages that are now wrong
These say a component cannot declare, expose or route a directory. Still true on the target until step 4: no route can end at a provider, because the root answers `NotSupported`. False about the manifest, which accepts it.

| Passage | Says | Now |
|---|---|---|
| `docs/guide/01-concepts.md:257` | "A component provides protocols. (Directories are the other kind, but the only one a route can lead to is the root's `boot`...)" | A component may declare and expose a directory, and a route may lead to it. |
| `docs/guide/01-concepts.md:374` | `AccessDenied`, for a directory use only, when an offer on the way gives fewer rights than the use asks for | An expose, and the provider's declaration, are hops too; the build warns. |
| `docs/guide/04-tree-servers.md:5` | "A manifest cannot route a directory that a component provides" | The manifest can. |
| `docs/guide/04-tree-servers.md:34` | "the manifest compiler refuses a component that declares or exposes a directory, and says the only directory is the root's built-in `boot` (`system/component_manifest.py:466`, `:649`)" | It does not, and in this commit the cited lines are other code (`:466` is the `startup` check, `:649` an offer's `type` refusal). |
| `docs/guide/04-tree-servers.md:445` | "A use of the root's `boot` states rights, and so does each offer on the way." | Each expose and the declaration state rights too, and the build walks the tree (warning). |
| `docs/guide/README.md:24`, `:39`, `:60`, `:119` | a tree server of your own "cannot yet be reached"; "a manifest cannot route a directory that a component provides" | The manifest side is open; the root's side is step 4. |
| `docs/guide/06-checklist.md:74` | "A manifest cannot declare or expose a directory, so no route reaches a tree server of your own" | The same. |
| `docs/guide/06-checklist.md:75` | "The manifest limit is Enforced at `system/component_manifest.py:466-470` and `:649-653`" | The limit is gone. |

### Guide cites that moved
Old lines are those the guide cites, true at `a0e63b77`; step 1 added about 15 lines to the compiler's data classes, so the cites into it were already off by that much when this step began. New lines are those of this commit.

| Guide line | What it cites | Old | New |
|---|---|---|---|
| `03-protocols.md:185` | "Without one, a protocol's type is its own name..." | `component_manifest.gni:109-111` | `component_manifest.gni:166-168` |
| `03-protocols.md:196` | the whole-tree check paragraph (now "twice") | `component_manifest.gni:62-65` | `component_manifest.gni:62-68` |
| `06-checklist.md:356` | the build refuses an expose of an undeclared capability | `component_manifest.py:664-671` | `component_manifest.py:739-746` (the directory case is the same lines) |
| `06-checklist.md:362` | `check_name` | `component_manifest.py:227-238` | `component_manifest.py:242-253` |
| `06-checklist.md:368` | `format_mismatch` | `component_manifest.py:1214-1233` | `component_manifest.py:1323-1342` |
| the same line | `_stated_type`, "a type goes with one protocol" | `:343-349` | `:358-364` |
| the same line | an offer takes no `type`; an expose takes none | `:563-568`; `:655-660` | `:649-650` (the message is built at `:623-637`); `:731-732` |
| `06-checklist.md:374` | `startup` must be written out | `:450-452` | `:465-467` |
| `06-checklist.md:380` | the program must install under `bin/` | `:411-415` | `:426-430` |

The cites of `realm.cpp`, `route.hpp` and `route.cpp` did not move in this step.

## For step 3
- The compiler's rules for a declared path are the ones `Outgoing::add_directory` has to hold again for a program that is wrong without a manifest: absolute, below `/`, no empty, `.` or `..` segment, not `/svc` or under it, and no declared path equal to, above or below another. The messages and the segment comparison are in `_capabilities` and `_is_prefix` (`system/component_manifest.py`); `/svcx` and `/export/data2` are legal and a string-prefix test refuses them.
- The `RouteShelf` and compiled-fixture tests give the root's step a tree to try `forward_directory()` on without a patched byte: `shelf.cm` with `store.cm` declares `/export/data` and exposes it three ways, and the expected `Open` for the working route is `export/data/shared/public/docs`.
- A directory that routes to a provider is not the only case that warns: dev's `root.cm` still prints one warning for `/denied` every time its action runs; ninja prints nothing when it does not.
