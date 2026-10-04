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
