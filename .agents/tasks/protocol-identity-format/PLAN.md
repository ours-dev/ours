# protocol-identity-format: manifest format version 2 and the `type` key, with no meaning yet

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 1 of 5 of `.agents/tasks/protocol-identity/PLAN.md`: add a string `type` to the compiled manifest's Capability and Use records and the `type` key to the manifest language, and give them no meaning. No check exists after this step; steps 2 to 4 add the run-time check, the build check and the root's report. The scope ends at the format, the key, the template parameter `type_check` and the compiler flag `--no-type-check`, both accepted and inert.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| The decoded view's type | (a) a field `std::string_view type`, filled with the name when the stored string is empty; (b) an accessor `type()` as the design says | (a): the decoded records are aggregates the router copies out of the bytes, so a field filled once by `detail::view()` is the same observable behaviour and needs no member function on them | Not asked | (a), by the plan (`.agents/tasks/protocol-identity/PLAN.md`, "Shared contract") |
| A list with `type` | (a) refused whatever its length; (b) refused only above one name, as `as` is | (a): the design says a `type` goes with a single protocol, not a list; a list of one that is later extended would silently give the new names the same type | Not asked | (a), by this step, within the design's rule |
| `builtins` template parameter | (a) added now; (b) with the build check in step 3 | (b): it has no reader until the compiler has `--builtins`, and the step's list names `type_check` and `--no-type-check` only | Not asked | (b), as the step's brief says |

## Steps
1. Read every reader and writer of the format. `grep -rn "OSCM\|component/format.hpp\|component_manifest"` over the sources finds one writer (`system/component_manifest.py`), one reader (`system/lib/component/src/decl.cpp`, through `format.hpp`) and the tests of both; nothing else decodes or encodes a `.cm`.
2. `format.hpp`: `VERSION` 2; `Capability` gains `String type` (24 bytes), `Use` gains `String type` before `rights` (56 bytes, `type` at 40, `rights` at 48); the static_asserts follow.
3. `decl.hpp`, `decl.cpp`: the decoded `Capability` and `Use` gain `std::string_view type`; the decoder bounds-checks the two new strings as it does every other; `detail::view()` fills the name in when the stored string is empty. `route.hpp`: `Builtin` gains `std::string_view type = {}`, empty meaning the name, after `rights` so no existing designated initialiser changes.
4. `component_manifest.py`: `VERSION` 2, the two struct strings, a `type` field on the `Capability` and `Use` dataclasses, `_stated_type()` and the places that call it, the refusals for a list, a directory use, an offer and an expose, `encode()` writing the string, `--no-type-check` accepted and ignored. `component_manifest.gni`: the `type_check` parameter forwarded as `--no-type-check`, and the documentation of the `type` key.
5. Fixtures: `provider.cm`'s capability states `type = "ours.example.Echo"`, and `consumer.cm`'s use of `echo`, the name `realm.cm` gives the protocol, states `type = "ours.example.Echo"`, so the decoder's test reads real bytes from the real template. The consumer's uses keep their order and count.
6. Tests: `system/test_component_manifest.py` for the key, its refusals and the bytes; `system/lib/component/test/decl_test.cpp` for the default, the explicit type, a faithful version-1 file, and a type string outside the table.
7. Verify as the Test plan says, then one commit: `feat(component): manifest format version 2 carries a protocol type`.

## Files
- `system/lib/component/include/component/format.hpp`, `decl.hpp`, `route.hpp`, `system/lib/component/src/decl.cpp` — the format, the decoder and `Builtin::type`
- `system/lib/component/test/decl_test.cpp`, `system/lib/component/test/fixtures/BUILD.gn` — the decoder's tests and the two fixtures that state a type
- `system/component_manifest.py`, `system/component_manifest.gni`, `system/test_component_manifest.py` — the compiler, the template and the compiler's tests
- `docs/changes/261029/component.md`, `docs/changes/261029/component-manifest.md` — the change records of the two halves
- `.agents/tasks/protocol-identity-format/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `python3 system/test_component_manifest.py` ends with `OK`.
- `prebuilt/ninja/ninja -C out/bringup.x86 host/obj/system/lib/component/test/decl host/obj/system/lib/component/test/route`, then `./host/obj/system/lib/component/test/decl` and `route` from `out/bringup.x86`: every test passes.
- clang-tidy on `decl.cpp` and `decl_test.cpp` reports nothing in the component library; clang-format and gn format are clean on the changed lines.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py`: both images build and boot, `routing_checks: passed` on rel, and the host tests pass. Every `.cm` of both images is version 2 now, so a boot is the proof that nothing still writes or reads version 1.
