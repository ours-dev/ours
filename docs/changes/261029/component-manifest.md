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

## Title: The build checks every protocol route of a manifest's tree and refuses one that ends at another protocol type (2026-10-29)

**Files**
- `system/component_manifest.py` — `compile_manifest()` is split into `_check_source()`, the rules a scope keeps before anything in it is read, and `_declare()`, the capabilities, uses, offers, exposes and cycle check, so a descendant's scope is read by the same rules; the type check: `Instance`, `route_protocol_use()` with `_find_builtin`, `_find_offer`, `_find_expose` and `_find_capability`, `build_tree()`, `check_types()`, `format_mismatch()`, `load_tree()`; `--tree` (required), `--builtins`, `--depfile`, and a `--no-type-check` that skips the check
- `system/component_manifest.gni` — `component_manifest_tree` and `component_manifest_tree_walk` metadata, the `<name>.tree` file, `--tree`, a `depfile`, the `builtins` parameter; the `type` paragraph and the `type_check` entry of the header, which said nothing checks yet, are rewritten
- `system/component_builtins.py` — new: `load()` validates the root's built-ins data file; the command line writes the C++ list `BUILTINS`
- `system/test_component_manifest.py`, `system/test_component_builtins.py` — the tests of both

**Symptom**
The experiment of 2026-10-29: `root.cm` offered the built-in `ours.process.Launcher` to `#pc-ps2` under the name `ours.resource.Irq`. The image built, booted, and the root logged nothing; pc-ps2 read `PeerClosed` from a Launcher that did not know its method. After this change the build stops at `root.cm.compile` and prints the offer.

**Cause**
Routing matches a `use` to an `offer` by kind and name, and a name is not a protocol once an `as` can rename it. Step 1 gave a capability and a use a `type`; this step is the first place that compares them, and it is the earliest one: the build has every manifest of the tree and the root's built-ins, and nothing is running yet to be confused.

The check is a second implementation of `route()`'s walk, in Python, because the root's is C++ and this runs in the build. Two copies of one walk drift: the day `route()` learns a new rule, a build that follows the old one either lets a mismatch through or rejects an image the root would route. So the copy is made as close as it can be. Each branch of `route_protocol_use()` is `route()`'s branch of the same name ("from is the parent, and the instance is the root", "from is the parent", "from is the child", "from is self"), with a comment that quotes the rule; the lookups are named for the `detail::find_*` functions and take the first match in table order as they do; and `FixtureRoutesTest` runs the walk over the fixture trees `route_test.cpp` routes through and compares, use by use, with what that test expects: which uses resolve and where they end. A mutation of the walk (no program check, an offer matched without its target child, a built-in matched without its kind) fails it.

It differs from `route()` in four deliberate ways. A protocol use, offer and expose state no rights and no subdir, so the hop rules have nothing to do, and a directory use is not walked at all: its hops are compared by rights, not by a type. A route that does not resolve is not an error, because routing_checks uses `ours.test.Unoffered`, which nobody offers, and a directory `denied`, which the root answers `AccessDenied`, on purpose, and the image has to build to be run. "The root" is the top of the tree being compiled, so a use that routes above it is compared with a built-in only when a `builtins` file was given, and skipped otherwise. And every child is resolved: the tree holds all the descendants, so `route()`'s "a child that cannot be resolved" has no counterpart; a child missing from the tree is a broken build.

Why a tree and not just the manifest. A use in `#a/#b` is satisfied by an offer in `#a`'s manifest, which `#a`'s parent made possible, and so on: no single manifest holds the route. The compile action therefore needs the declaration of every descendant, and that is what a trap is in. GN can give an action the metadata of its dependencies as a generated file, but not as `inputs`: the names are known only inside the metadata walk, which a variable of the action cannot read. So `.tree.json` lists every descendant's `.source.json`, the compiler reads them, and writes a `depfile` naming each, which ninja records and compares on the next run. (The root's compile also depends on its children's compile outputs through the phony targets of their groups, so it reran after a descendant's change before there was a depfile; that is a side effect of how groups chain, and the depfile makes the dependence on the declarations themselves explicit.)

The `.source.json` of a child is built in the user toolchain while the root's is in the default one, so a path cannot be derived from the root's `target_gen_dir`. Each manifest's own metadata therefore carries its own path, rebased to `root_build_dir` where the compiler runs, taken from its own `target_gen_dir`; and the key is walked with `walk_keys`, a list of the children's groups, since GN's default walk would go on into every child's program and everything it depends on.

**Change**
The template: every group now carries the label and scope path of its manifest and the list of its children's groups to walk next, and `<name>.tree` collects them:

```gn
# after: in the group's metadata
component_manifest_tree = [
  {
    label = _label
    source = rebase_path(_source_json, root_build_dir)
  },
]
component_manifest_tree_walk = _child_deps

generated_file(_tree_target) {
  deps = _child_deps
  data_keys = [ "component_manifest_tree" ]
  walk_keys = [ "component_manifest_tree_walk" ]
}
```

The existing `component_manifest` key keeps its barrier (`component_manifest_barrier = []`): a parent's `.children` still collects its own children and no more. The action takes `--tree`, writes the depfile when the check is on, and takes `builtins` as `--builtins` and as an `inputs` entry, so editing the data file reruns every check that reads it. `type_check = false` passes `--no-type-check`, and then no depfile is asked for and the tree and the built-ins are not read.

The error is the design's, generalised. For the experiment it is exactly:

```text
error: //kernel/products/image/dev:root.cm: #pc-ps2 uses protocol "ours.resource.Irq" (type ours.resource.Irq),
       but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
         use    #pc-ps2 "ours.resource.Irq"
         offer  root.cm "ours.process.Launcher" from parent to #pc-ps2 as "ours.resource.Irq"
       Offer a capability of type "ours.resource.Irq" under that name, or give the use the type it means.
```

and for a provider two exposes down, where the instance is named by its path, `#svc/#impl`, and a hop that renames carries `as`:

```text
error: //test:root.cm: #client uses protocol "ours.example.Hello" (type ours.example.Hello),
       but the route ends at #svc/#impl's capability "ours.example.Echo" (type ours.example.Echo):
         use    #client "ours.example.Hello"
         offer  root.cm "ours.example.Service" from #svc to #client as "ours.example.Hello"
         expose #svc "ours.example.Echo" from #impl to parent as "ours.example.Service"
         expose #svc/#impl "ours.example.Echo" from self to parent
       Offer a capability of type "ours.example.Hello" under that name, or give the use the type it means.
```

Each line of the route is a hop in the words of the manifest that declares it, named by the instance whose manifest it is (the root by its own file name, `root.cm`). There is no closing `capability` or `built-in` line: the design's example, which a test pins byte for byte, has none, and the sentence above the route already names the end and its type. The message does not carry the `component_manifest:` prefix of the compiler's other refusals, since it begins `error:` as the example does. All mismatches of the tree are printed, ordered by instance path and then by the order of the uses, before the compiler exits.

`component_builtins.py` reads the data file of the shared contract and refuses, in the compiler's own words, an unknown key, a missing `name` or `kind`, a bad name or type, a `type` on a directory, `rights` on a protocol, a bad rights word, and a repeated name. It writes `constexpr auto BUILTINS = std::array{component::Builtin{...}, ...};` in the file's order, with every name fully qualified and no `#include`, since the file will sit inside a namespace.

**Reference**
None, this project's own design. Fuchsia has no type on a protocol, so it has no counterpart for the build check; its closest is `scrutiny`, a validator that walks the component tree's routes before an image ships, of which this walk is the smallest part (see `.agents/tasks/protocol-identity/DESIGN.md`). The walk follows `component::route()` in `system/lib/component/include/component/route.hpp`.

**Verification**
- `python3 system/test_component_manifest.py`: 99 tests OK (63 before); `python3 system/test_component_builtins.py`: 26 tests OK. Python 3.9.6, standard library only.
- New in the first: the experiment's text and a provider-side text, byte for byte; the experiment through the command line with a built-ins file (exit 1, no output file, the text on stderr); a renamed use with its type stated, and without; a route through two offers and an expose, with its hops; a capability that states a type; a built-in that states one; a use in the root itself; a manifest under two parents; mismatches ordered by instance path and use; nine routes that do not resolve (offered by nobody, a child that exposes nothing, from self with no program, a provider with no program, offered to another child, an offer of another kind, above the top with no built-ins, with built-ins that lack the name, with one of another kind); a directory use; `--no-type-check`, which does not even read the tree; the depfile's content, and that a failed check writes none; the tree's errors; and the cross-check over the named fixtures.
- Built with the check on: the fixtures (`realm.cm`, `top.cm`, which has two levels below it, `mismatch.cm` built with `type_check = false`) and the whole image, whose `root.cm` has four children and no `builtins` yet. `decl` 20 tests and `route` 41 tests pass.
- Scratch, reverted: routing_checks' `type = "ours.examples.echo.Echo"` changed to `"ours.examples.echo.EchoWrong"` fails the image build with the message naming `#routing_checks`, `"ours.test.Echo"`, `#echo` and the offer from `#echo`; a harmless capability added to `echo.cm` reruns `root.cm.compile`; a depfile naming a file newer than the output makes ninja rerun it.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, `routing_checks: passed`), host tests PASS (148 passed, 0 failed).

**Deliberately not done**
- `builtins` on `root.cm` and the data file itself: step 4. Until then a route in the image that ends at a built-in is not checked at build time, which includes the experiment's own case; the unit tests give the compiler a built-ins list of their own.
- A closing `capability` or `built-in` line in the error, and a check of directory uses. The first is redundant; the second is not a type question.
- A fix of a route that does not resolve: left to run time on purpose.
- A shared description of the walk for the two implementations to be generated from. Two copies with a cross-check were chosen as the smaller step. The cross-check pins the Python walk to a table copied by hand from `route_test.cpp`, not to `route()` itself, so a change to `route()` that also changes that test is not caught until the table is edited; a table both read would remove the drift, and is worth doing if the walk grows.

**How it was found**
None. It is the third step of the plan accepted on 2026-10-29.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A route does not resolve because nobody offers the capability. Why is that not a build error, when a route that resolves to the wrong type is?
2. Why does the compile action need a depfile when GN already knows every child's `.source.json` path, and what would have run in the wrong toolchain if `.tree` had been built from the root's `target_gen_dir`?
3. `FixtureRoutesTest` compares the Python walk with a table copied from `route_test.cpp`. If someone changes `route()` and updates `route_test.cpp`, which test of this step fails, and what must they do to keep the two walks equal?
