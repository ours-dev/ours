# component-manifest changes

## Title: The manifest language has a `type` key on a protocol capability and use, and the compiler writes format version 2 (2026-10-29)

**Files**
- `system/component_manifest.py` — `VERSION` is 2; `CAPABILITY` is `<IIIIII` (24 bytes) and `USE` is `<IIIIIIIIIIIIQ` (56 bytes); the `Capability` and `Use` dataclasses gain `type: str = ""`; `_stated_type()` reads and checks the key; `capabilities` and `use` accept it; an `offer`, an `expose` and a directory `use` refuse it with their own messages; `encode()` writes the string; `--no-type-check` is accepted and ignored
- `system/component_manifest.gni` — the `type_check` parameter (default true) forwards `--no-type-check` to the compiler when false; the template's header documents the `type` key and the parameter
- `system/test_component_manifest.py` — tests for the key, each of its refusals, the name rules, the layout of the new strings, and the bytes written for an omitted and an explicit type

**Symptom**
Latent, cannot be triggered today: the key is accepted and written, and nothing compares it. Before this change `type` was an unknown key, so a manifest that stated one failed with `unknown key 'type'`, and there was no way to say what protocol a renamed capability was.

**Cause**
The language had no word for the identity of a protocol, so the compiled format had nothing to carry (see `docs/changes/261029/component.md`), and the script could not have checked a route for it even if it had wanted to. The key and the format field arrive together because the library's tests decode bytes this script writes: the two halves of the format must both exist before either can be tested against the other.

**Change**
The key goes on the two places where a protocol is stated:

```text
capabilities = [ { protocol = "ours.examples.echo.Echo" } ]
use = [ { protocol = "ours.test.Echo", type = "ours.examples.echo.Echo" } ]
```

Without `type` a protocol's type is its own name, so a manifest that never renames anything is unchanged. The rules, each with its refusal:

| Where | Rule | Refusal |
|---|---|---|
| `type` value | follows `check_name()`: not empty, a name's characters, at most 100 bytes, a string without NUL | the same messages as any name, at `<clause>.type` |
| `capabilities` or `use` with a list | a type goes with one protocol, so a list is refused even of one name | `` `type` goes with a single protocol, not a list; write a clause for each protocol that needs one `` |
| a directory `use` | a directory has no type: its hops are compared by rights | ``a directory use has no `type`; only a protocol has one, and a directory's hops are compared by their rights`` |
| an `offer` | it passes a capability on and `as` renames the capability, not the protocol | `` an offer has no `type`: `as` renames the capability, not the protocol. State the type where the capability is declared (`capabilities`) and where it is used (`use`) `` |
| an `expose` | the same | the same, for an expose |

A list of one name is refused with `type`, though `as` accepts one. `as` renames what a list has one of; a type would give every name the list grows to the same type, and a clause that was right with one protocol would silently be wrong with two. The offer and expose checks run before the unknown-key check, so the message names `as` and does not say only `unknown key 'type'`.

`encode()` writes the string after `subdir` in a Use and after `name` in a Capability:

```python
# before
tables += CAPABILITY.pack(capability.kind, 0, *strings.add(capability.name))
# after
tables += CAPABILITY.pack(
    capability.kind, 0, *strings.add(capability.name), *strings.add(capability.type)
)
```

An omitted type is stored as the empty string, `(offset, 0)`, which adds nothing to the string table; an explicit one equal to the name adds nothing either, because `_Strings.add()` stores each text once, and its record points at the name's own bytes. The tests pin both.

`type_check` and `--no-type-check` are added now so that step 2's fixture, which must build a mismatched tree on purpose to exercise the run-time check, can name the parameter. The template forwards the flag only when the parameter is false, so the default command line is unchanged. The compiler accepts the flag and does nothing with it; the check it names is step 3. The template's text says so.

Rejected: a `builtins` template parameter in this step. It has no reader until the compiler takes `--builtins`, which is step 3's, so adding it now would be a parameter that is accepted and dropped with nowhere to be tested.

**Reference**
None, this project's own design. Fuchsia's `cm_rust` has no type on a protocol use or capability (see `docs/changes/261029/component.md`); the key and its rules follow `.agents/tasks/protocol-identity/DESIGN.md`.

**Verification**
- `python3 system/test_component_manifest.py`: 63 tests OK (48 before). New: the key on a capability and a use, an explicit type equal to the name, an omitted type being empty, a list refused (of two and of one, on both), a directory use, an offer (three shapes) and an expose refused, the name rules at both places, the format being version 2, the offsets `type` sits at (16 in a Capability, 40 in a Use, with `rights` at 48), the bytes of an omitted type, of an explicit type, of a type equal to the name, of a directory use, and `--no-type-check` leaving the output byte for byte as it was.
- Under Python 3.9.6, standard library only.
- The compiler's bytes are decoded by `decl` in the library's host tests, from real `component_manifest()` targets: see `docs/changes/261029/component.md`. `gn gen` and a build of both images accept the template change, and `routing_checks: passed` on the rel image.
- `scripts/format_gn.py --check system/component_manifest.gni system/lib/component/test/fixtures/BUILD.gn`: clean.

**Deliberately not done**
- The type check itself, and `--builtins`, `--tree` and the depfile it needs: step 3.
- No migration of an existing manifest: none of the images' states a `type`, because nothing checks. `routing_checks`' renamed use of `ours.test.Echo` will need one once the check is on, and step 4 adds it with the other manifests the check shows need one.
- The template's text says that nothing checks for a mismatch yet. Step 3 changes those two sentences with the check.

**How it was found**
None. It is the first step of the plan accepted on 2026-10-29.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is `type` refused on an `offer` with a message about `as` rather than allowed as an override of the type the route carries?
2. A manifest states `type = "p"` on a capability named `p`. Which bytes does the compiler write that differ from the same manifest without `type`, and which are the same?
3. Why does `type_check = false` exist before the check does, and what is the one kind of target it is for?
