# protocol-identity-format: manifest format version 2 and the `type` key, delivered with no meaning

**Date**: 2026-10-29

## Delivered
Step 1 of `.agents/tasks/protocol-identity/PLAN.md`, one commit on the branch `pi-format`, `feat(component): manifest format version 2 carries a protocol type` (its hash is in the report that handed this step back, since a commit cannot name itself):

- `format::Capability` is `{kind, reserved, name, type}`, 24 bytes, and `format::Use` is `{kind, flags, source, source_name, target_path, subdir, type, rights}`, 56 bytes with `type` at 40 and `rights` at 48. `VERSION` is 2 in `format.hpp` and in `component_manifest.py`, and the decoder refuses version 1 with NotSupported.
- An empty `type` string means the entry's own name. The decoded `component::Capability` and `component::Use` carry `std::string_view type`, which `detail::view()` fills with the name when the stored string is empty. `component::Builtin` carries `std::string_view type = {}`, empty meaning the name.
- The manifest language takes `type` on a single protocol in `capabilities` and in `use`, with the rules of a name. It is refused on a list, on a directory use, on an `offer` and on an `expose`; the last two say that `as` renames the capability, not the protocol.
- `component_manifest()` takes `type_check` (default true) and passes `--no-type-check` when it is false; the compiler accepts the flag and does nothing with it.
- `provider.cm`'s capability and `consumer.cm`'s use of `echo` state a `type`, so `decl_test.cpp` reads real bytes from the real template.
- Change records: `docs/changes/261029/component.md` (the library half) and `docs/changes/261029/component-manifest.md` (the compiler half). Both are append-only; later steps add sections.

## Verification
- `python3 system/test_component_manifest.py`: 63 tests OK under Python 3.9.6 (48 before).
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then each run from `out/bringup.x86`: `decl` 20 tests passed (four new), `route` 32 tests passed unchanged.
- clang-tidy on `decl.cpp` and `decl_test.cpp` against `out/bringup.x86`: nothing outside `vendor/googletest`. `scripts/format_cpp.py` and `scripts/format_gn.py`: clean on every changed file. `scripts/lint_docs.py --changed` on each of the four documents: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-format`: build ok in 6.6 s; dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines (`routing_checks: passed`, `init: echo exited with 0`, `init: holding the IRQ and I/O port resources`, `pc-ps2: ready`, `pc-ps2: key a`, `pc-ps2: key A`); host tests PASS, 148 passed, 0 failed. Every `.cm` under `out/bringup.x86` (root, shell, pc-ps2, echo, routing_checks, and the fixtures) reads back as magic `OSCM`, version 2.

## Deviations from the design
- The decoded view has a field `std::string_view type`, filled with the name when the stored string is empty, where DESIGN.md says an accessor `type()`. The records are aggregates copied out of the bytes one at a time, so a field filled once by `detail::view()` is the same observable behaviour. This is the plan's own deviation (`.agents/tasks/protocol-identity/PLAN.md`, "Shared contract").
- A directory use decodes with `type` equal to its name too, since `detail::view()` fills every empty type. The design says a type goes with a protocol and nothing reads a directory use's; the filled-in name keeps the promise that a reader never sees an empty type, and the manifest language refuses a `type` on a directory use, so it is never stated.
- `Builtin::type` is the last member of `Builtin`, after `rights`: declaration order fixes the order of designated initialisers, and this keeps every existing initialiser as written. A list that states both writes `.rights` before `.type`.
- The `builtins` template parameter of the shared contract is not added in this step. The step's brief names `type_check` and `--no-type-check` only, and `builtins` has no reader until the compiler takes `--builtins` in step 3.
- A `type` with a list is refused even for a list of one name, though `as` accepts one: the design says a type goes with a single protocol, not a list.

## Found along the way
- Three texts say the check does not exist yet and become false in step 3: the `type` paragraph and the `type_check` entry in `system/component_manifest.gni`'s header ("nothing checks for one yet", "Accepted now and without effect until the build check exists"), and the help of `--no-type-check` in `component_manifest.py` ("the check is not built yet, so this changes nothing"). Step 3 must rewrite them with the check. This is recorded in `docs/changes/261029/component-manifest.md`, "Deliberately not done".
- `decl_test.cpp`'s `RefusesABadHeader` used version 2 as its example of an unsupported version; with `VERSION` 2 it now uses 1 and 3. Nothing else in the tree named a version.
- No defect found that is not fixed, so there is no `docs/repairs/261029/` file.

## Not done
- No comparison of types anywhere (steps 2 and 3), no report of a mismatch in the root's log (step 4).
- `system/lib/component/README.md`, `docs/design/261004/component-framework.md` and `docs/architecture/component-framework/` do not describe the new field yet: step 5.
- No manifest of an image states a `type` yet; `routing_checks`' renamed use of `ours.test.Echo` needs one once the check is on, which is step 4.
