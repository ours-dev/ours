# protocol-identity: five steps that make a mismatched protocol route a build error and a logged BadType

**Date**: 2026-10-29
**Status**: Proposed

## Request
The user asked on 2026-10-29 to fix the "same name, different protocol" gap found by the experiment in DESIGN.md, with subagents, to choose the point at which a mismatch is stopped with care, and to make the error output say what is wrong. They chose a string `type` on the capability declaration, and on 2026-10-29 accepted all five recommendations in DESIGN.md. The scope ends at protocols: directories keep their rights check, messages keep their hand-written codecs, and the id and `switch` of `docs/repairs/261028/init.md` item 1 stay a separate change.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| The five decisions of DESIGN.md | See DESIGN.md | All (a) | Not asked | All (a), 2026-10-29 |
| How the steps are ordered | (a) the format first and alone, then the run-time check and the build check in parallel, then the root; (b) library and compiler in parallel from the start | (a): the library's tests decode bytes the compiler wrote, so both halves of the format must exist before either can be tested | Not asked | (a), by the plan; the user may redirect |
| Where the design lives | (a) here until the last step, then `docs/design/261029/protocol-identity.md`; (b) moved now | (a): the steps link to it | Not asked | (a), by the plan |

## Steps
Each step has a workspace `.agents/tasks/protocol-identity-<name>/` with its own `PLAN.md` (the step as done, from the section below) and `RESULT.md` (the skeleton in `.agents/tasks/TEMPLATE.md`), a worktree made with `python3 scripts/new_worktree.py <branch> <start>`, and one commit, made as `git commit -- <paths>` and listing its own files. The commit is on the step's branch; the orchestrator integrates it. Every step follows CLAUDE.md: English only; `cpp-write-and-verify` and `modern-cpp-development` for C++; no mention of Zircon, Fuchsia or Linux in code comments; comments say what a thing is and why; a change record in `docs/changes/261029/<module>.md` in the same commit; `scripts/lint_docs.py --changed` on every document touched; `ours-verify` before the commit where the step says so.

The order is 1, then 2 and 3 in parallel, then 4. Step 2 and step 3 touch disjoint files.

### Shared contract
Fixed here so that the steps agree before any of them is written.

**Format version 2.** `VERSION` is 2 in `format.hpp` and in `component_manifest.py`; a version-1 file is refused with NotSupported, as any other version is.

- `format::Capability` is `{Kind kind; u32 reserved; String name; String type;}`, 24 bytes. In Python, `CAPABILITY = struct.Struct("<IIIIII")`.
- `format::Use` is `{Kind kind; RecordFlags flags; Ref source; String source_name; String target_path; String subdir; String type; Rights rights;}`, 56 bytes, `rights` at offset 48. In Python, `USE = struct.Struct("<IIIIIIIIIIIIQ")`.
- An empty `type` string means "the entry's own name". The writer writes an empty string when the manifest omits `type`, so an unchanged manifest carries no new bytes. An explicit type equal to the name is legal and means the same.
- The decoder bounds-checks `type` as it does every other string.
- The decoded views `component::Capability` and `component::Use` gain `std::string_view type`, which `detail::view()` fills with the name when the stored string is empty, so a reader never sees an empty type. (The design says an accessor `type()`; a field filled once is the same observable behaviour and fits these aggregates. Record it in RESULT.md as a deviation.)
- `component::Builtin` gains `std::string_view type = {}`, where empty means the name. `route()` reads it through one helper, so hand-made `Builtin` lists in tests keep working.

**The manifest language.**

- `capabilities = [{ protocol = "n", type = "t" }]` and `use = [{ protocol = "n", type = "t" }]`. `t` follows the name rules of `check_name()`.
- `type` goes with a single protocol: a list with `type` is a build error. A `type` on a directory entry, or on an `offer` or an `expose`, is a build error; the message for an offer or expose says that `as` renames the capability, not the protocol.
- The `component_manifest()` template gains `type_check` (default true) and `builtins` (optional file path). `type_check = false` is for fixtures that exercise the run-time check and for nothing else; it reaches the compiler as `--no-type-check`. `builtins` is a source-absolute path, passed as `--builtins` and listed in the action's `inputs`; only `root.cm` gives it.

**The built-ins data file**, `system/service/init/builtins.json`, owned by step 4 and read by step 3:

```json
{
  "builtins": [
    {"name": "boot", "kind": "directory", "rights": ["rx*"]},
    {"name": "ours.process.Launcher", "kind": "protocol"},
    {"name": "ours.resource.Irq", "kind": "protocol", "type": "ours.resource.Irq"}
  ]
}
```

`type` is optional and defaults to the name; `rights` is allowed on a directory only and uses the words of `component_manifest.py`'s `RIGHTS`. Step 3 writes `system/component_builtins.py`: `load(path)` validates the file and returns the list, and its command line writes the C++ include file that defines `constexpr auto BUILTINS = std::array{component::Builtin{...}, ...};` in the order of the file. Step 4 includes that file where `BUILTINS` is defined today.

**The run-time error.** `RouteError` gains `std::string_view want{}` and `got{}`, set only for a type mismatch. For that error `status` is `BadType`, `verb` is `Declare`, `at` is the providing instance (nullptr for a built-in) and `name` the capability's name at the source. The comparison is made once, when the walk ends, and only for `Kind::Protocol`.

**The build error and the log line** are the texts in DESIGN.md, "Error output". The step that writes one pins its exact text in a test.

### 1. Format version 2 and the `type` key
Background: every manifest in the image is a file in this format, so both halves change together, and nothing else can be tested before they do. This step adds the field and the key and gives them no meaning yet: no check exists after it.

- Branch `pi-format`, from the integration branch's head. Workspace `protocol-identity-format`.
- C++: `format.hpp`, `decl.hpp`, `decl.cpp`, and `Builtin::type` in `route.hpp`. Python: `component_manifest.py` (version, structs, dataclass fields, the `type` key and its errors, `encode()`), `component_manifest.gni` (the `type_check` parameter forwarded to the compiler, which accepts and ignores `--no-type-check` for now; the documentation of the `type` key). Fixtures: give one fixture a capability and a use with an explicit type, so that the decoder test sees it.
- Tests: `system/test_component_manifest.py` (the key, the errors, the bytes); `system/lib/component/test/decl_test.cpp` (default and explicit type, version 1 refused, an out-of-range type string refused).
- Look for every other reader or writer of the format first (`grep -rn "OSCM\|component/format.hpp\|component_manifest"`) and change each.
- Verify: `python3 system/test_component_manifest.py`, the component host tests, then `ours-verify`'s `boot_check.py` in the step's worktree: every manifest in both images is version 2 now, so a boot is the proof that nothing still writes or reads version 1.
- Commit: `feat(component): manifest format version 2 carries a protocol type`.

### 2. The run-time check in `route()`
Background: DESIGN.md's second interception, the first Open. `route()` is a template shared by the root and the host tests, so the check lives in the walk's last step and nothing else changes.

- Branch `pi-runtime`, from step 1's commit. Workspace `protocol-identity-runtime`.
- `route.hpp` (the compare, `RouteError::want` and `got`, the spec comment of `route()`: errors list, invariants), `src/route.cpp` if a helper goes there, `test/route_test.cpp`, `test/fixtures/BUILD.gn` and `test/BUILD.gn` for a fixture tree that mismatches on purpose (`type_check = false`).
- Cases, from DESIGN.md "Testing": same type routes; a provider mismatch answers `BadType` with `want` and `got`; the default is the name; a renamed use with an explicit type routes; a built-in mismatch (a `Builtin` list with another `type`); a directory use is not compared; a route that fails for another reason keeps its old status.
- Verify: the component host tests, and a build of everything that includes `route.hpp` (`grep -rn "component/route.hpp"`).
- Commit: `feat(component): route() stops at a capability of another protocol type`.

### 3. The build-time check
Background: DESIGN.md's first interception. A manifest with children checks its whole tree; the check resolves each protocol use the way `route()` does and compares types, and leaves a route that does not resolve alone, so the deliberate failures of routing_checks (`ours.test.Unoffered`, `denied`) still build.

- Branch `pi-build`, from step 1's commit. Workspace `protocol-identity-build`.
- `component_manifest.gni`: each manifest group's metadata gains a `component_manifest_tree` entry `{label, source}` with no barrier; a `<name>.tree` generated file collects it for the whole tree; the compile action passes it (`--tree`) and the `builtins` file, and writes a `depfile` listing every `.source.json` it read, so that a change in any descendant reruns the check. `component_manifest.py`: `--tree`, `--builtins`, `--no-type-check`; the walk (up through offers, down through exposes, to a capability of a component with a program, or to a built-in of the same kind and name; a use walked above the top of the tree with no `builtins` given, or a route that does not resolve, is skipped); the error text of DESIGN.md. `system/component_builtins.py` and its test `system/test_component_builtins.py`.
- Cross-check against `route()`: list the rules of the walk in the script's comments by the names of route()'s spec, and add a test over the fixture trees of `system/lib/component/test/fixtures` (the ones that resolve) that every route they resolve passes.
- Tests in `system/test_component_manifest.py`: a mismatch fails with the exact text; a renamed use with an explicit type passes; a route through two offers and an expose; a route that does not resolve builds; the experiment of 2026-10-29 as a fixture (the Launcher offered as `ours.resource.Irq`); `--no-type-check` skips the check; a depfile is written.
- Verify: `python3 system/test_component_manifest.py`, `python3 system/test_component_builtins.py`, and `gn gen` plus a build of the fixtures' manifests (`//system/lib/component/test:manifests`) in the step's worktree. `root.cm` does not pass `builtins` until step 4.
- Commit: `feat(component_manifest): the build rejects a route that ends at another protocol type`.

### 4. The root: built-ins, the log line, the manifests
Background: the last piece, where the two earlier checks meet the image. The root's built-ins become one list that the compiler and `realm.cpp` both read, `log_route_error()` names a type mismatch, and the manifests that rename a capability say what it is.

- Branch `pi-root`, from the integration branch's head with steps 2 and 3 in it. Workspace `protocol-identity-root`.
- `system/service/init/builtins.json`; its GN action running `component_builtins.py`; `realm.cpp` includes the generated list in place of the hand-written `BUILTINS`, with a `static_assert` for each name that a library also defines (`ours_process::LAUNCHER_NAME`, the `ours_resource` protocols' names) so that the two cannot drift; `log_route_error()` prints, for `BadType`, `init: <moniker>'s use of "<name>" reaches another protocol: it wants <want>, the route ends at <got> (<where>)`, where `<where>` is `the built-ins` or `provided by <moniker>`. The epitaph is already `routed.unwrap_err().status`, so a mismatch closes the channel with `BadType` without further code.
- `kernel/products/image/dev/BUILD.gn`: `root.cm` gains `builtins = "//system/service/init/builtins.json"`. `system/service/init/test/routing_checks/BUILD.gn`: its use of `ours.test.Echo` becomes its own clause `{ protocol = "ours.test.Echo", type = "ours.examples.echo.Echo" }`. Check every other manifest of the image still builds with the check on; each fix is a `type` where a name is renamed.
- The experiment of 2026-10-29, again, in a scratch worktree under `.worktrees/`, never committed: (a) `root.cm` offers the Launcher to `#pc-ps2` as `ours.resource.Irq`: the build must fail with the message of DESIGN.md; (b) the same with `type_check = false` on `root.cm`: the image boots, and the log must show the init line and pc-ps2's `getting the IRQ resource: BadType`. Copy both outputs into the change record as the evidence.
- Verify: `ours-verify` on both images with `routing_checks: passed`; the host tests; `grep -n "BUILTINS" system/service/init/realm.cpp` finds no hand-written list.
- Documents: `docs/changes/261029/init.md`, and `docs/repairs/261028/init.md` item 1 rewritten for what remains (the built-ins' list is generated; `connect()` still matches by name).
- Commit: `feat(init): the root reports a protocol mismatch, and its built-ins are one generated list`.

### 5. Documents and the design's move
Done by the orchestrator after step 4: `docs/design/261004/component-framework.md` (the `type` key in D3, one paragraph and a pointer), `docs/architecture/component-framework/glossary.md` (protocol type), `system/lib/component/README.md` (format version 2, the checks), `docs/architecture/component-framework/libraries.md` if it describes the format, DESIGN.md moved to `docs/design/261029/protocol-identity.md` with its links fixed, this PLAN's status set to Done, and a `RESULT.md`. Commit: `docs(component): protocol type, the design accepted and the checks described`.

## Files
- Step 1: `system/lib/component/include/component/{format,decl,route}.hpp`, `system/lib/component/src/decl.cpp`, `system/lib/component/test/{decl_test.cpp,fixtures/BUILD.gn}`, `system/component_manifest.{py,gni}`, `system/test_component_manifest.py`, `docs/changes/261029/{component,component-manifest}.md`, the step's workspace.
- Step 2: `system/lib/component/include/component/route.hpp`, `system/lib/component/src/route.cpp`, `system/lib/component/test/{route_test.cpp,BUILD.gn,fixtures/BUILD.gn}`, `docs/changes/261029/component.md`, the step's workspace.
- Step 3: `system/component_manifest.{py,gni}`, `system/component_builtins.py`, `system/test_component_manifest.py`, `system/test_component_builtins.py`, `docs/changes/261029/component-manifest.md`, the step's workspace.
- Step 4: `system/service/init/{builtins.json,BUILD.gn,realm.cpp}`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, any manifest the check shows needs a `type`, `docs/changes/261029/init.md`, `docs/repairs/261028/init.md`, the step's workspace.
- Step 5: the documents named above.

## Test plan
- Step 1: `python3 system/test_component_manifest.py` passes; the `decl` host test passes; both images boot with `routing_checks: passed`.
- Step 2: the `route` host test passes with the new cases; every target that includes `route.hpp` builds.
- Step 3: both Python test files pass; the fixtures' manifests build; the build error for the experiment's fixture is exactly the text in DESIGN.md.
- Step 4: `ours-verify` passes both images; the scratch experiment shows the build error for (a), and for (b) the init line and `getting the IRQ resource: BadType`; the repo contains no hand-written `BUILTINS` list.
- After step 4, on the integration branch: all host tests (`scripts/host_tests.py` lists them), `ours-verify` on both images, `scripts/lint_docs.py` over every changed document.
