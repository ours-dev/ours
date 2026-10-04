# component changes

## Title: component_manifest() compiles a GN scope into meta/<name>.cm, and system/lib/component decodes it (component-framework step 7) (2026-10-04)

**Files**
- `system/component_manifest.gni` — new: the `component_manifest()` template, five targets per manifest (source JSON, program name, children, compile, group), the user toolchain added to the program and child labels, and the `distribution_entries` that put `meta/<name>.cm` into a BootFS
- `system/component_manifest.py` — new: the compiler, cmc's per-manifest checks over the subset of D3, and the writer of the flat format (D2); it fixes the name length at 100 bytes (the design's open question) and writes child URLs as `fuchsia-boot:///#meta/<name>.cm` (D26)
- `system/test_component_manifest.py` — new: 48 unit tests, one or more per rule, the layout, and `main()`
- `system/lib/component/include/component/format.hpp` — new: the layout, the enums and the limits the compiler shares, with static_asserts on sizes and offsets
- `system/lib/component/include/component/decl.hpp`, `src/decl.cpp` — new: `component::Decl::decode()`, structural checks only, and the decoded records, with strings as views of the manifest
- `system/lib/component/test/decl_test.cpp`, `test/BUILD.gn`, `test/embed_manifests.py` — new: the gtest, which decodes the manifests the template built from the fixtures, carried in the binary as byte arrays
- `system/lib/component/test/fixtures/BUILD.gn`, `fixture_program.cpp` — new: four fixture manifests, `realm.cm` read in the default toolchain and its three children in the user toolchain, and the program two of them name
- `system/lib/component/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — scaffolded with copier from `tools/templates/cpp_library`, then filled in
- `BUILD.gn` — `//system/lib/component:tests` in the host test list
- `.agents/tasks/component-framework/RESULT.md` — new: the manual gn gen checks the plan asks to record there

**Symptom**
None, a planned change: step 7 of `.agents/tasks/component-framework/PLAN.md`. Before it, nothing in the tree could say which components exist, what each one uses and who offers it; the router (step 8) and the root (step 13) read what this writes.

**Cause**
Four things here are worth learning: where the checks run, why the template has to name a toolchain, how GN metadata carries a name from one target to another, and why the format leaves no byte with two readings.

The objects:
- **The manifest target**: `component_manifest("shell.cm") { program = ":shell" manifest = { use = [...] } }` in a BUILD.gn. The scope uses CML's key names (D1); what is a GN label in ours (the program, each child) is a template parameter, so it is resolved by GN and a dangling one fails `gn gen`.
- **The compiler** (`component_manifest.py`): ours's cmc. It reads three JSON files GN wrote and writes `$target_gen_dir/<name>.cm`.
- **The compiled manifest** (`meta/<name>.cm` in the BootFS): a 48-byte header, seven fixed-size record tables, one string table.
- **The decoder** (`Decl::decode()`): what the root (step 13) runs on a file it read from its own /boot.

The events, for `root.cm` in an image's BUILD.gn naming the child `//examples/shell:shell.cm`:
1. `gn gen` reads the image's BUILD.gn in the default toolchain. The template appends `(//build/toolchain/ours:user-<cpu>)` to the child's label, so GN reads `examples/shell/BUILD.gn` in the user toolchain, where `shell.cm` and the program `:shell` are defined.
2. `root.cm.source` writes the scope as JSON. `root.cm.children` walks GN metadata from each child's group and writes `[{label, path: "meta/shell.cm"}]`. `shell.cm.program` walks the metadata of `:shell.install_name` and writes `[{path: "bin/shell"}]`.
3. ninja runs the compiler on each, children first, since a parent's compile depends on its children's groups.
4. The BootFS's metadata walk (`distribution_entries`) collects `meta/root.cm`, `meta/shell.cm` and `bin/shell`: depending on `root.cm` packages the whole tree.
5. At boot (step 13) the root reads `meta/root.cm`, decodes it, and finds child `shell` at `fuchsia-boot:///#meta/shell.cm`.

Why the semantic checks run only at build time. A manifest is checked twice in Fuchsia: cmc runs cml's checks and `cm_fidl_validator::validate` when it compiles, and the resolver runs `validate` again on every resolve, because a package can come from anywhere a resolver can reach. ours has one source of manifests, the BootFS, and only the build writes it. So the compiler enforces every rule once (unique names, `from: "self"` naming a declared capability, no offer cycle, use paths that do not nest, known rights words, `startup` written out, the program under `bin/`, D3's subset), and the decoder checks only what memory safety needs: that every table and the string table fit the bytes exactly, that every string reference lies inside the string table, that every enum value and flag is one the format defines, that reserved fields are zero, and that every child index resolves. The root is the most privileged process on dev; a decoder that trusted a count or an offset would let one corrupt file read outside its buffer there. A decoder that trusts a name's uniqueness can at worst route wrongly, and the build has already refused that file.

Why the template adds a toolchain. In GN a label without a toolchain resolves in the toolchain of the file that names it, and each BUILD.gn is evaluated once per toolchain that something references it in. An image's BUILD.gn is read in the default toolchain (`//build/toolchain/ours:kernel-<cpu>`, which carries the host environment for actions). If `root.cm` named `//examples/shell:shell.cm` plainly, GN would read `examples/shell/BUILD.gn` in that toolchain too, and its `assert(toolchain_environment == "user")` would fail gen; without the assert it would compile the shell with `-mcmodel=kernel`. `user_bootfs()` met the same problem for programs and solved it the same way: add the user toolchain once, and refuse a label that already names one, since a second suffix is not a label GN can parse. The test fixtures exercise exactly this: `realm.cm` is built in the default toolchain and its children in the user one.

Why names come through metadata. A program's BootFS path is decided by `user_executable()`'s `install_name` (`bin/<target>` by default), and a child's meta path by its own target name. Spelling either again in the parent would let the two drift apart, and the drift would show only at boot as a NotFound. GN's metadata walk carries them instead: a `generated_file()` with `data_keys` collects a key from every target reachable through its deps. Two barriers keep each walk to what it should see. `walk_keys = ["component_manifest_barrier"]` with an empty barrier list in every manifest group stops `root.cm.children` at each child, so a grandchild's entry never lands in the root's list; this is how `fuchsia_component_manifest()` stops its `component_manifest_path` walk. And `distribution_entries_barrier = []` on the compile action keeps the BootFS walk out of the JSON targets, whose deps reach the children a second way.

Why every byte has one meaning. A child Ref carries the child's index; a parent or self Ref must carry zero there. A record's rights word must be zero unless its flags say rights are stated. Reserved fields must be zero, and the tables must end exactly where the file does. A field that may hold anything when unused is a field two readers can disagree about: one ignores it, a later version gives it a meaning, and old files suddenly mean something new. Refusing the stray value now keeps that door shut. The rights flag is there for a real distinction, not tidiness: in Fuchsia's rights walk, a hop that states no rights narrows nothing and the walk skips it (`validate_next`), while a hop that stated an empty set would allow nothing. One u64 cannot say both, so the flag says which.

Why the name length is 100. Fuchsia has two answers. `fuchsia.component.decl` declares `name` as `string:100`, the bound the persisted manifest is encoded against; cm_types' `Name`, which cml validates with, takes fuchsia.io's 255. A 101-byte name passes cml and then cannot be persisted, so 100 is the limit a compiled manifest really has, and ours's compiled form is the counterpart of that one. It also keeps every protocol name inside the one 255-byte path segment it becomes under /svc.

Why the records are copied out. `Records::operator[]` memcpys each record from the bytes rather than casting a pointer: the manifest arrives in whatever buffer a Read filled, with no promised alignment, and no `format::Offer` object was ever created in those bytes, so reading through a cast pointer would be undefined behaviour twice over.

Why an iterator holds the view, not the Records (a review fixup to this step). `decl.offers()` returns a new `Records` each call, a view like `std::span`. The first version's `Iter` held a pointer to the `Records` that made it and compared that pointer. Two consequences. `auto it = decl.offers().begin();` kept a pointer to a temporary destroyed at the end of the statement, so `*it` read a dead object. And `std::find_if(decl.offers().begin(), decl.offers().end(), pred)` compared iterators from two different temporaries, which never matched: a search that found nothing walked past the table and hit the bounds trap. That is the router's natural search in step 8. `Iter` now copies the view's three words and compares the table address and the index, so iterators behave like `std::span`'s pointers: positions in the bytes, whichever view handed them out.

**Change**
The template, adding the toolchain and refusing one already named:

```gn
assert(
    string_replace(child.manifest, "(", "") == child.manifest,
    "component_manifest($target_name): a child's `manifest` takes a plain label without a toolchain suffix: ${child.manifest}")
_child = get_label_info(child.manifest, "label_no_toolchain")
_child_deps += [ "$_child($_user_toolchain)" ]
```

The layout, the same on both sides:

```python
HEADER = struct.Struct("<IHHIIIIIIIIII")  # 48 bytes
USE = struct.Struct("<IIIIIIIIIIQ")  # 48 bytes
OFFER = struct.Struct("<IIIIIIIIIIIIQ")  # 56 bytes
```

```cpp
struct Offer final {
    Kind kind;
    RecordFlags flags;
    Ref source;
    String source_name;
    Ref target;
    String target_name;
    String subdir;
    Rights rights;
};
static_assert(IS_RECORD<Offer> && sizeof(Offer) == 56);
```

The decoder's string check, in 64 bits so that an offset near 4 GiB cannot wrap back inside:

```cpp
if (static_cast<u64>(string.offset) + string.size > bounds.strings) {
    return Status::OutOfRange;
}
```

Every table starts where the one before ends, so the header's counts alone place them, and `layout.end != data.size()` refuses both a short file and trailing bytes.

Rejected:
- Embedding the fixtures with `#embed`: Clang 23 still takes it in C++ only as an extension (`-Wc23-extensions`), which `-Werror` refuses. A 60-line Python script writes the byte arrays into a header instead, so the test runs from any directory.
- Reading the fixtures at run time from paths baked in as defines: the test would then depend on where the out directory is.
- Child references by name in the compiled form: the design asks for "indices that resolve", and an index spares the router a string comparison per hop.
- Writing the manifest's own `runner` and `binary` into `manifest.program`: both come from the template's `program` label, so the compiler refuses them there rather than let two spellings disagree.

**Reference**
- Fuchsia's `fuchsia_component_manifest()` (`build/components/fuchsia_component.gni:82-148`): a manifest target that compiles with cmc and packages `meta/<name>.cm` as a resource, with `component_manifest_path` metadata and its barrier.
- cmc's `compile()` (`tools/cmc/src/compile.rs:18-82`), which loads the .cml, runs `cml::compile()` and persists the result with `fidl::persist`; `cml::compile()` (`tools/lib/cml/src/translate.rs:90-163`), which ends in `cm_fidl_validator::validate`.
- The rules: cml's `validate.rs` for duplicate use targets (`:641`), nested use paths (`:692-745`), duplicate expose targets (`:940`), duplicate offer targets, keyed on the name alone (`:1312-1326`), and duplicated rights (`:1635`); `cm_fidl_validator`'s strong dependency cycle check (`src/sys/lib/cm_fidl_validator/src/lib.rs:413-421`); cm_types' `validate_name` (`src/sys/lib/cm_types/src/lib.rs:348-365`).
- The values: `fuchsia.component.decl/MAX_NAME_LENGTH` and `MAX_PATH_LENGTH` (`sdk/fidl/fuchsia.component.decl/types.fidl:6-8`), `StartupMode` (`child.fidl`), and `fuchsia.io/Operations` with its `R_STAR_DIR`-style aliases (`sdk/fidl/fuchsia.io/rights-abilities.fidl:9-62`), spelled by cml's `Right` (`tools/lib/cml/src/types/right.rs:19-41`).
- The re-validation ours drops: `src/sys/lib/routing/src/resolving.rs:133-143`. The boot URL: `fuchsia_boot_resolver.rs:45-46` (D26).
- In ours: `user_program_names()` (`system/user_program_names.gni`) for reading an install name from metadata, `user_bootfs()`'s caller in `kernel/products/image/dev/BUILD.gn` for adding the user toolchain, and `generated_resource()` (`build/dist/generated_resource.gni`) for the `distribution_entries` shape.

**Verification**
- `python3 system/test_component_manifest.py`: 48 tests, OK. Each rule refused, with its accepted counterpart where one exists: `lazy` and a missing `startup`, a protocol use with `path`, a declared and an exposed directory, a label that names a toolchain, names of 100 and 101 bytes, nested and duplicate use paths (`/data` and `/database` accepted), an offer cycle (a diamond accepted), duplicate offer targets across kinds, the layout of every record, and `main()` writing nothing on a refusal.
- `out/bringup.x86/host/obj/system/lib/component/test/decl`: 15 tests, PASSED. The four fixtures decoded field by field; every truncation of `realm.cm` refused; trailing bytes, a bad magic, version 2 (NotSupported), two programs, arguments without a program, a string one byte past the table and one at offset 0xFFFFFFFF (OutOfRange), a child index of 3 among 3 (OutOfRange), a parent Ref with an index, unknown kinds, flags, rights bits and runner, LAZY's 0, rights on a protocol offer and every reserved field refused; and every one-byte change to each fixture, to seven values each, either refused or decoded with every string inside the buffer and every child index in range. Indexing past the end traps (a death test).
- Review fixup: `Decl.IteratorsAreViewsOfTheTable` failed on the first version (`decl.offers().end() == decl.offers().end()` was false, then the `find_if` trapped) and passes after it; the test binary now runs 16 tests, all PASSED. `boot_check.py` after the fixup: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped).
- `gn gen` refusals, checked by hand and recorded in RESULT.md: a child naming a target that does not exist ("Unresolved dependencies ... needs ...:ghost.cm(//build/toolchain/ours:user-x86)"), a child label and a program label each spelled with a toolchain (the template's asserts).
- Packaging, checked by hand and reverted: with `fixtures:realm.cm` added to dev's `user_bootfs` deps, `bootfs.fini` listed `meta/realm.cm`, `meta/provider.cm`, `meta/consumer.cm`, `meta/hollow.cm` and `bin/fixture_program` once.
- `src/decl.cpp` compiled with the user toolchain's flags too, where the root will link it in step 13.
- clang-tidy over `decl.cpp`, `decl_test.cpp` and `fixture_program.cpp` (headers included): nothing reported.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed), rel PASS (22 passed, 3 skipped, 0 failed), the same counts as before the change. No image carries a manifest yet, so the boot shows only that nothing else moved.

**Deliberately not done**
- `route()`: step 8, written by the user (P3).
- No image packages a manifest yet; `root.cm` arrives with the root in step 13.
- No semantic checks in the decoder (see Cause); no check there that names are non-empty or free of NUL, which the compiler refuses.
- The Python unittest is not run by the build; it is run by hand, as the plan says. The C++ test is built by `//:default` and run by hand, like the other host tests.
- No depfile for the compiler: its inputs are the three JSON files GN writes, all listed as sources.
- Two manifests with the same target name in one image collide at `meta/<name>.cm`; the distribution manifest's collision check refuses that, not the compiler.

**How it was found**
None, a new feature.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The root decodes `meta/root.cm` without checking that child names are unique. Why is that safe, and what would have to change in how manifests reach the root before it stops being safe?
2. If `component_manifest()` did not add the user toolchain to a child's label, what exactly would happen at `gn gen` for `root.cm` in `kernel/products/image/dev/BUILD.gn`, and what would happen if `examples/shell/BUILD.gn` had no assert?
3. Why does an offer record need a RightsStated flag beside its rights word, and what would a route through a hop with `rights = 0` and no flag mean under each reading?

## Title: route()'s spec, the types around it and failing tests; the function itself is pending, written by the user (component-framework step 8, preparation) (2026-10-04)

**Files**
- `system/lib/component/include/component/route.hpp` — new: the `Instance` concept, `Builtin`, `Verb`, `Subdir`, `Route` and `RouteError`, and `route()`, whose comment is the whole spec. Its body is a stub marked `// Written by the user (P3): see the spec above.` that answers NotSupported. **Pending: written by the user (P3)**
- `system/lib/component/src/route.cpp` — new: `Subdir::prepend()` and `Subdir::view()`, the subdirectory a route accumulates
- `system/lib/component/test/route_test.cpp` — new: 30 host tests, 5 `Subdir.*` that pass and 25 `Route.*` that fail until `route()` exists, over an instance tree of the test's own that resolves children on demand
- `system/lib/component/test/fixtures/BUILD.gn` — three new manifests, `top.cm` (a second root, with `realm.cm` one level down), `user.cm` and `leaf.cm`, each route the tests check named in a comment on the declaration that decides it; `realm.cm` is now visible to its own directory, since `top.cm` names it as a child
- `system/lib/component/test/BUILD.gn` — the three manifests embedded beside the four of step 7, and the `route` executable
- `system/lib/component/BUILD.gn` — `src/route.cpp`, `route.hpp`, and `test:route` in the `tests` group
- `system/lib/component/README.md` — the router and its test
- `BUILD.gn` — the comment on `//system/lib/component:tests` in the host test list says the router's tests fail until `route()` is written
- `docs/repairs/261004/component.md` — new: the copies of the 1 KiB `Subdir` every route makes, a performance observation

**Symptom**
None: a planned preparation step, step 8 of `.agents/tasks/component-framework/PLAN.md`. After step 7 the root can decode every manifest, but nothing can answer the question each `use` asks: who provides this, with what rights, under which subdirectory. The root needs that answer in step 13, once per directory use when a component starts (D8), and in step 14 on every Open of `/svc/<name>` (D11). P3 reserves the function that answers it, `route()`, for the user to write by hand under a time limit; this step writes everything around it.

**Cause**
What routing is, as objects and then events.

The objects:
- **The target**: the instance whose manifest has the `use`. Its decl is decoded, since it was resolved before it could be started.
- **The use**: kind (protocol or directory), source (always the parent, D3), source name, and for a directory its rights and subdir.
- **An offer**: a record in a parent's manifest, "give capability `source_name` from `source` (parent, self or `#child`) to `#target` as `target_name`", with rights and a subdir for a directory.
- **An expose**: a record in a child's manifest, "give my parent `source_name` from `source` (self or `#child`) as `target_name`".
- **A capability declaration**: "I serve `name` from my outgoing directory at `svc/<name>`", meaningful only with a program to serve it.
- **The built-ins**: what `from: "parent"` means at the root, which has no parent. On dev, `boot` (a directory, rx*), `fuchsia.process.Launcher` and `fuchsia.kernel.DebugResource`.
- **The walk's state**: where it is (`at`), where the capability comes from as seen from there (`from`), and its name there (`name`); plus the last rights a hop stated, and the subdir collected so far.

The events, for `leaf`'s use of `echo` in the test's `top.cm` tree, the longest route the tests take:
1. Start at `user/leaf`, from the parent, name `echo`.
2. In `user`'s offers, the one to `leaf` named `echo` comes from the parent as `fuchsia.example.RealmEcho`. Now at `user`, from the parent, name `fuchsia.example.RealmEcho`.
3. In `top`'s offers, the one to `user` by that name comes from `#realm`. Now at `top`, from the child `realm`, same name.
4. Resolve `realm`. Its expose to the parent by that name comes from `#provider` as `fuchsia.example.Echo`. Now at `realm`, from the child `provider`.
5. Resolve `provider`. Its expose of `fuchsia.example.Echo` comes from self. Now at `realm/provider`, from self.
6. `provider` has a program and declares `fuchsia.example.Echo`: the route ends there. The root will forward the Open to `svc/fuchsia.example.Echo` in provider's outgoing directory.

Two levels up, two levels down, renamed twice. Nothing in leaf's manifest says where `echo` lives; each manifest knows only its own neighbours, which is what lets a component be moved or replaced without touching its users.

Why the walk is bounded. Going up, each step moves to a parent, so it takes at most the target's depth. Once an offer comes from a child the walk turns down, and from then on an expose's source can only be self or a child, so each step goes one level deeper. Nothing turns it back up, so the walk ends within twice the depth of the tree. That argument rests on "no expose comes from a parent", which the compiler guarantees but the decoder does not check: step 7 decided the decoder checks structure only. A manifest with an expose from the parent would decode, and a walk that followed it could go up, find the parent's offer that leads back down, and loop for ever inside the root, the one process every other process waits on. So the spec makes that expose an error, NotFound at the child, which keeps the bound true for everything the decoder accepts. Fuchsia's `ExposeSource` has no `Parent` variant at all (`src/sys/lib/cm_rust/src/lib.rs:1139-1146`), so the rule costs nothing in fidelity. The same reasoning gives the walk its start: it begins from `use.source`, not from a hard-coded parent, so a decodable use from self is walked by the same rules instead of being silently routed as if it came from the parent.

Why rights are held against the hop before, not against the use. Walking from the use toward the source, each hop that states rights must contain the rights stated by the last hop before it that stated any: the sequence may only grow. In the test's `top.cm` tree, `leaf` asks r* of `wide`; `user` offers `wide` as rx*, from what `top` offered it as `thin`, r*. Against the use alone, every hop has r*, and the route would pass. But `user` offered rx*, more than it was given, and a later change to `leaf`'s use (to rx*) would then fail at `top` with no change in `top` or `user`: the error would surface far from the mistake. Fuchsia's `WalkState::advance()` checks each hop against the one before for exactly this reason, and so does the spec, answering AccessDenied at `top`, where the narrowing offer is. A hop that states no rights narrows nothing and is skipped, which is why step 7 keeps a RightsStated flag beside the rights word: "no rights stated" and "an empty set of rights" mean opposite things here. The route's result carries exactly the use's rights, never a hop's: the walk only proves that every hop had at least that much, and the connection carries what the component asked for.

Why subdirs are prepended. A use of `config` with subdir `ours`, offered as `boot` with subdir `config`, offered as `boot` with subdir `etc`, means: the source's `etc`, within it `config`, within that `ours`. The walk meets them in the opposite order, use first, so each hop's subdir goes in front, as Fuchsia's `DirectoryState::advance()` calls `subdir.extend(self.subdir)` with the hop's subdir as the receiver. A joined path longer than MAX_PATH_LENGTH answers NotFound, as Fuchsia's `PathTooLong` does.

Why every lookup failure is NotFound, and the root's status mapping is so coarse. The status goes into an epitaph on the client's channel. Fuchsia maps nearly every routing failure to NOT_FOUND and a rights failure to ACCESS_DENIED (`RoutingError::as_zx_status()`), including a child whose manifest fails to resolve (`ComponentInstanceError::ResolveFailed`): the client cannot act on "your provider's manifest is corrupt", and should not learn the shape of the tree from the status. The detail goes into the root's log instead, which is what RouteError's instance, verb and name are for: "NotFound at realm: Expose fuchsia.example.Unexposed" says which manifest to fix.

Where an error points. The design says the error names "the moniker where the walk stopped" and the verb; the spec makes that the owner of the declaration that was missing or too narrow: the parent for an offer, the child for an expose, the instance itself for a declaration, and nullptr for the built-ins. Fuchsia's `RightsRoutingError::Invalid` names the requesting side instead (the moniker of the hop before). The spec follows the design's wording, since the owner is the manifest someone has to change. The built-ins answer with Declare, not Offer, because component_manager keeps them as capability declarations (`builtin_capabilities()` is a list of `CapabilityDecl`), searched as a component's own declarations are.

Why a template over a concept. The router is pure and tested on the host, but it has to read an instance tree that belongs to the root (step 13): the root's instance record also holds a process handle, an outgoing channel and a state the router has no business seeing. Fuchsia solves this with a trait, `ComponentInstanceInterface`, which component_manager's instance and the routing tests' model both implement, and the router is generic over it. The C++ counterpart is a concept with five members, `parent()`, `index()`, `decl()`, `resolve()` and `child()`, and `route()` a function template over it, so the dispatch is decided at build time, as this repo prefers to `virtual`.

Why `Subdir` is a 1 KiB buffer inside the result. User programs have no heap (`system/lib/ort/src/cpp_dependencies.cpp:10-26`), and the joined subdir is a new string no manifest contains. Fuchsia's `RelativePath` allocates a new string at each `extend()`. `Subdir` holds MAX_PATH_LENGTH bytes inline and writes the path from the end of the array toward its start, so a prepend copies only the new head. The price is a 1 KiB copy each time a Route is moved, recorded in `docs/repairs/261004/component.md`.

How the tests pin the spec down. Each expectation compares one line describing the whole answer, such as `NotFound at realm: Expose fuchsia.example.Unexposed` or `built-in boot, subdir "etc/config/ours", rights r*`, so a wrong answer prints everything it got wrong at once. The tests' instance tree resolves a child only when asked, and turns a broken precondition, such as calling `decl()` on an instance nobody resolved, into a test failure rather than a crash. Most rules have a declaration of their own in the fixtures. Seven tests cannot be written that way, because the compiler refuses a manifest that shows the rule (yet `route()` must be defined on everything the decoder accepts), or because the manifest would be unreadable: they change the bytes of a compiled manifest before it is resolved. They are an expose from the parent, an expose to something other than the parent, an expose and a declaration of the wrong kind, a use from self, a subdir one byte past the limit and one exactly at it (a string appended to the string table), and a corrupt child manifest.

**Change**
The public shape (`component/route.hpp`):

```cpp
template <typename T>
concept Instance = requires(T &instance, T const &view, u32 const index) {
    { view.parent() } -> std::same_as<T *>;
    { view.index() } -> std::same_as<u32>;
    { view.decl() } -> std::same_as<Decl const &>;
    { instance.resolve() } -> std::same_as<ours::Status>;
    { instance.child(index) } -> std::same_as<T &>;
};

struct Builtin final { Kind kind; std::string_view name; ustl::Option<Rights> rights; };
enum class Verb : u32 { Offer = 1, Expose = 2, Declare = 3 };

class Subdir final { /* prepend(head) -> bool, view(), is_empty() */ };

template <typename C>
struct Route final { C *provider; std::string_view name; Subdir subdir; ustl::Option<Rights> rights; };

template <typename C>
struct RouteError final { ours::Status status; C *at; Verb verb; std::string_view name; };

template <Instance C>
auto route(C &target, Use const &use, std::span<Builtin const> builtins)
    -> ustl::Result<Route<C>, RouteError<C>>;
```

And the function left to the user, as committed:

```cpp
template <Instance C>
auto route([[maybe_unused]] C &target, [[maybe_unused]] Use const &use,
           [[maybe_unused]] std::span<Builtin const> const builtins)
    -> ustl::Result<Route<C>, RouteError<C>> {
    // Written by the user (P3): see the spec above.
    return ustl::err(RouteError<C>{
        .status = ours::Status::NotSupported,
        .at = nullptr,
        .verb = Verb::Declare,
        .name = {},
    });
}
```

Where the spec fills in what the design leaves open, each without touching a decision of the design's table:
- At the root, a name that is no built-in answers NotFound at nullptr with Declare; the design names no verb for it.
- A child that cannot be resolved answers NotFound at that child with Expose, whatever status its resolver gave.
- An expose from the parent answers NotFound at the child (see Cause), and an expose counts only when its target is the parent, as `find_matching_exposes()` requires.
- The walk starts from `use.source`, which is the parent for every use the compiler writes.
- Within a hop, rights are checked before the subdir, and the first failure on the walk is the answer; among several matching records, the first in table order counts.
- Whether the provider has exited (D23) is the caller's check, not the walk's.

Rejected:
- An abstract base class for the instance, with virtual members: a vtable for a tree whose concrete type is fixed in each program, and a base class this repo asks the user about first.
- An `Instance` class owned by this library, which the root would embed: it would have to carry the resolver as a callback, and step 13 would inherit a record shape decided before the root exists.
- RouteError holding the moniker as a string: there is no heap to build it in, and the caller has the instance to print it from.
- A hop counter as the termination guard instead of the rule about exposes from the parent: it would bound the loop but answer an arbitrary error at an arbitrary hop, where the rule names the manifest at fault.
- A caller-supplied buffer for the subdir: one more parameter on every call for a cost not measured yet; the repair item keeps the question open.

**Reference**
Fuchsia's directory router until 2025-07-24, at `9e015f591c4^` (the commit that replaced it is `9e015f591c4`, "Impl Directory bedrock routing"):
- `route_directory()` (`src/sys/lib/routing/src/lib.rs:930-980`), whose walk from a use (`:952-975`) builds a `DirectoryState` from the use's rights and subdir and calls `route_from_use()` (`legacy_router.rs:56`).
- From the parent: `Use::route()` (`legacy_router.rs:652-698`) and `Offer::route_segment()` (`:1084-1132`), with `find_matching_offers()` (`:1555-1574`) matching the kind, the target name and the target child (`target_matches_moniker()`, `:1518`); at the root, `find_builtin_source()` (`:541-569`) over the top instance's `CapabilityDecl` list.
- Into a child: `change_directions()` (`:1144-1186`) and `Expose::route_segment()` (`:1479-1506`), with `find_matching_exposes()` (`:1576-1594`) matching the kind, the target name and `ExposeTarget::Parent`.
- From self: `Offer::route_segment()` (`:1025-1064`) and `Expose::route_segment()` (`:1445-1459`), over `find_component_source()` (`:576-601`).
- The hop rules: `DirectoryState::advance()` and `finalize()` (`lib.rs:833-867`), over `WalkState::advance()` and `finalize()` (`walk_state.rs:45-98`); `RightsWalker::validate_next()` (`rights.rs:93-104` there, `rights.rs:19-33` at `51f05104c13`); `RelativePath::extend()` (`src/sys/lib/cm_types/src/lib.rs:928-942` at `51f05104c13`).
- The instance interface: `ComponentInstanceInterface` (`src/sys/lib/routing/src/component_instance.rs:22`), `try_get_parent()` (`:52`) answering `AboveRoot` (`:330-332`), and `lock_resolved_state()` (`:63`), which resolves first.
- The statuses: `RoutingError::as_zx_status()` (`src/sys/lib/routing/src/error.rs:405-441` at `51f05104c13`), `RightsRoutingError::Invalid` as ACCESS_DENIED; a capability with no program to serve it, `make_program_outgoing_connector_router()` (`src/sys/component_manager/src/model/component/instance.rs:785-792`) answering `InstanceNonExecutable`, NOT_FOUND (`src/sys/component_manager/lib/errors/src/lib.rs:230`).

Zircon has no counterpart: routing is component_manager's, a user program.

**Verification**
- Host tests, today, with the stub: `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/component/tests && out/bringup.x86/host/obj/system/lib/component/test/route` prints `[  PASSED  ] 5 tests.` (the `Subdir.*` tests) and `[  FAILED  ] 25 tests`, every `Route.*` test, each first failure reading `Which is: "NotSupported at the built-ins: Declare "` against the expected line. The decoder's test is unchanged: 16 tests, PASSED.
- The tests were checked against a reference `route()` written in a scratch copy and not committed: all 30 passed. Then against 24 mutants of that reference, each a plausible mistake: rights held against the use instead of the hop before, a stated-nothing hop resetting the rights, the hop's rights returned instead of the use's, the use's subdir dropped, subdir overflow ignored, offers matched by source name, or ignoring kind, target child, or with the use's instance's index, AccessDenied named at the requester or with the next name, built-ins ignoring kind or rights, built-in errors with Offer, a built-in route named by the use, children not resolved, the resolver's status passed on, an expose from the parent followed, exposes ignoring kind or target, no program check, declarations ignoring kind, and a walk that starts from the parent. Every one failed at least one test.
- The scratch reference also found a mistake in the first version of the tests: they named `leaf` where its moniker is `user/leaf`.
- `src/route.cpp` compiled with the user toolchain's flags, and a translation unit instantiating `route()` over a minimal instance type compiled there too, with the stub and with the reference body.
- clang-tidy over `src/route.cpp` and `test/route_test.cpp`, `route.hpp` included: nothing reported.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed), rel PASS (22 passed, 3 skipped, 0 failed), the same as before: nothing on the target calls `route()` until step 13.

**Deliberately not done**
- `route()`'s body: P3 reserves it for the user, under a time limit, reviewed afterwards as a strict interviewer would. Once it is written, `docs/authored.md` gets its row and the plan's commit `feat(component): route a use to its source` can land.
- Nothing to wire into the boot: the router has no caller on the target until the root (step 13), so the failing tests are host tests only and boot_check cannot see them.
- No test where both a hop's rights and its subdir fail: the spec says rights first, and no fixture can make both fail at one hop without also failing earlier.
- No test of duplicate matching records ("the first in table order"): the compiler refuses duplicates, and the rule is there only to make the answer deterministic.
- `.agents/tasks/component-framework/RESULT.md` is not updated here: other steps land in parallel, and the integrator records them together.

**How it was found**
None, a new feature.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A manifest that decodes may contain an expose whose source is the parent. Trace what a walk that followed it would do in the root, and explain why refusing that one record is enough to bound every walk.
2. `leaf` asks r* of `wide`, `user` offers it rx*, and `top` offered `user` only r*. Why does the route answer AccessDenied at `top` although every hop holds r*, and what later change would have surfaced the mistake far from where it was made if the check were against the use alone?
3. Why is the subdir collected by prepending, why does the route carry the use's rights rather than the source's, and what would a client be able to do with each if it got the other?
