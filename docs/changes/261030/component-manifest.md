# component-manifest changes

## Title: The compiler writes format version 3, with a Capability of 40 bytes and an Expose of 56, and leaves the new fields empty (2026-10-30)

**Files**
- `system/component_manifest.py` — `VERSION` is 3; `CAPABILITY` is `<IIIIIIIIQ` (40 bytes: kind, flags, name, type, path, rights at 32) and `EXPOSE` is an `OFFER`'s `<IIIIIIIIIIIIQ` (56 bytes: subdir at 40, rights at 48); the dataclasses `Capability` (`path`, `rights`) and `Expose` (`subdir`, `rights`) gain the fields with empty defaults; `encode()` writes them
- `system/test_component_manifest.py` — the sizes and the version, the fields at the offsets `format.hpp` fixes (read without the structs that wrote them), every compiled capability and expose with the new fields empty, a directory capability and a directory expose built by hand and read back, and the three refusals kept

**Symptom**
Latent, cannot be triggered today: no manifest the language can write states a path, a subdir or rights on a capability or an expose, so every new field is written empty and the only visible change is that every `.cm` is 16 bytes bigger for each capability and each expose it holds and says version 3. What it prepares for is the compiler change of the next step, which lets a manifest say them; without this step that change would be a format change and a language change in one diff, and a failing boot would not say which of the two broke it.

**Cause**
The format has two writers' halves and they must agree byte for byte: `format.hpp` and the Python `struct` strings, each with a twin of every constant. `component::format::Capability` and `Expose` grew (see `docs/changes/261030/component.md`), so the writer has to grow with them or the C++ decoder reads the second record of every table from the middle of the first. The decoder refuses a version-2 file, so a writer left at 2 would not even boot: `init` would fail to decode `root.cm`.

An empty string costs no byte. `_Strings.add("")` records the offset of the end of the table so far and size 0, and a string that is already in the table is not added again, so a capability that states no path adds no byte to the string table: `test_an_omitted_type_writes_an_empty_string` still sees the table `b"pq/svc/q"`. The only growth of a `.cm` is therefore the records: 16 bytes for each capability and 16 for each expose.

**Change**
```python
# before                                               # after
CAPABILITY = struct.Struct("<IIIIII")  # 24            CAPABILITY = struct.Struct("<IIIIIIIIQ")  # 40
EXPOSE = struct.Struct("<IIIIIIIIII")  # 40            EXPOSE = struct.Struct("<IIIIIIIIIIIIQ")  # 56, an Offer's layout
```

`encode()` writes the flags and rights of a capability and an expose through the same `rights()` helper a use and an offer go through: `None` writes no flag and a zero word, any number writes `FLAG_RIGHTS_STATED` and the mask, so `0` is a stated empty set and not an unstated one (`test_a_directory_capability_and_expose_encode_at_the_offsets_format_hpp_gives` pins the difference). The `Capability` and `Expose` dataclasses take the new fields as trailing defaults, so every existing construction, positional ones in the tests included, is unchanged and compares equal to what the parser builds.

The language is not opened. `compile_manifest()` still refuses a declared directory, an exposed directory and an offer of a directory from self or a child, and the parser never sets the new fields. `test_the_language_is_not_opened_by_the_format_change` pins that with the full spelling the design uses (a directory with `path` and `rights`, an expose with `rights` and `subdir`, an offer from `self` and from a child), so a later step that removes a refusal has to change that test on purpose. The rejected option was to write the parser's half now, the keys and the path rules, with the format change: it is the next step, with its own tests and its own boot.

**Reference**
None for the writer, which is this project's own half of a contract; the decoder's change record names the Fuchsia counterparts of the fields.

**Verification**
- `python3 system/test_component_manifest.py` with `OURS_BUILD_DIR` at the build: 102 tests (99 before; two replaced, five added), all pass, `FixtureRoutesTest` included, which compiles the fixtures through the same `encode()`.
- The C++ `decl` test decodes every fixture this script wrote (`realm.cm`, `provider.cm`, `consumer.cm`, `hollow.cm`) in version 3, so the two halves are checked against each other, not only each against its own table.
- Mutation checks on the writer, each made to a copy, run and reverted: the version left at 2, the type and path swapped, the rights not written, the capability flags not written, the expose flags not written, the expose subdir written as the target name, the expose rights not written, the old 40-byte expose layout, the old 24-byte capability layout, and rights placed first. Each is caught: the two layout mutations by eight and eleven tests, the version by one, the others by one to three.
- `boot_check.py --out out/dc-step1`: dbg 44 passed, 4 skipped, 0 failed; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed; the same as before. All 15 `.cm` files of the build read back as version 3 with the sizes above.

**Deliberately not done**
The parser's keys (`path` and `rights` on a directory declaration, `directory`, `rights` and `subdir` on an expose), the path rules, the non-overlap rule and the build-time rights walk: the next step. No manifest of this repository changes.

**How it was found**
None. A new feature, from the design.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `encode()` write a stated empty set of rights differently from none, and who reads the difference?
2. Why can the writer and the decoder not be changed one after the other, as two commits?
3. A capability that states no path adds no byte to the string table: why, and what in the tests would notice if it did?
