# directory-capability-step1: format version 3, and the expose and declaration hops of a directory route

**Date**: 2026-10-30

## Delivered
Branch `dc-step1`, worktree `.worktrees/dc-step1`, one commit on `a0e63b77`: `feat(component): format version 3, and the expose and declaration hops of a directory route`. It is step 1 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md`:

- `component`: `format::VERSION` is 3. `format::Capability` is 40 bytes (kind 0, flags 4, name 8, type 16, path 24, rights 32) and `format::Expose` is 56 (kind 0, flags 4, source 8, source_name 16, target 24, target_name 32, subdir 40, rights 48, an `Offer`'s layout). `RecordFlags` takes the place of the reserved word of both, and `RightsStated` is the flag offers and uses already use. The decoder bounds-checks the two new strings, puts the flags and rights of both records through `check_rights()`, refuses a version-2 file with NotSupported and no longer checks a reserved word on those two records. `component::Capability` gains `path` and `rights`, `component::Expose` gains `subdir` and `rights`.
- `route()`: an expose is a hop (AccessDenied or NotFound at the child, verb Expose); the declaration of a directory is the last hop (AccessDenied at the provider, verb Declare), followed by the check that the declared path and the joined subdir fit one Open (NotFound at the provider, verb Declare). `Route` gains `path`, the declared path of a directory and empty for a protocol and a built-in. The spec comment and the error list are rewritten. The type check is still protocol-only. `detail::open_path_fits()` is new in `route.cpp`.
- `system/component_manifest.py` writes version 3 with the new fields empty for every manifest there is; its `Capability` and `Expose` dataclasses carry the fields with empty defaults, so step 2 only has to fill them. The compiler still refuses a declared directory, an exposed directory and an offer of a directory from self or a child.
- `realm.cpp` is untouched and compiles: nothing forced a use of the new fields, and a directory that ends at a provider is still NotSupported (step 4).
- Documents: `docs/changes/261030/component.md` and `docs/changes/261030/component-manifest.md` (both new files), the library README, this note.

## Verification
- `boot_check.py --out out/dc-step1` against the same command on the untouched `a0e63b77` (`out/dc-step1-base`): identical counts.

  | | Before | After |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines | the same |
  | host tests | PASS, 151 passed (executables), 0 failed | the same |

- Host tests of the library: `decl` 20 to 26 tests, `route` 41 to 63, all pass. `python3 system/test_component_manifest.py` with `OURS_BUILD_DIR=out/dc-step1`: 99 to 102 tests, all pass, `FixtureRoutesTest` among them (it ran, it did not skip).
- Mutation checks, each applied to a copy of the source, built, run and reverted (the scripts are in the session scratchpad, not in the tree): 12 on `decl.cpp` (D1 to D12), 16 on `route.hpp` and `route.cpp` (R1 to R16), 10 on `component_manifest.py` (P1 to P10). Every one is caught by at least one test; the results are in the change records. Two of them are worth naming here because they are the rules this step exists for: R3 (the length checked before the rights at the declaration) is caught only by `TheRightsOfTheDeclarationAreCheckedBeforeTheLength`, and R5 (a protocol declaration also a hop) only by `ADeclarationOfAProtocolIsNoHopAndStatesNoPath`. A mutation of `format.hpp`'s field order does not need a test: the static_asserts stop the compile.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step1`) on `decl.cpp`, `route.cpp`, `decl_test.cpp`, `route_test.cpp`: no diagnostic on any line of `system/lib/component`; what remains is googletest's header. `format_cpp.py --check` is clean on all seven C++ files. `lint_docs.py --changed` is clean on both change records.
- The `.cm` files: all 15 the build wrote read back as version 3, each with the old size plus 16 bytes per capability and per expose (`root.cm` 1369, unchanged, since it holds neither; `echo.cm` 166 to 198; `hello.cm` 169 to 201; `routing_checks.cm` 1062, unchanged). The five the dev image packages (`root`, `echo`, `pc-ps2`, `routing_checks`, `shell`) are in `bootfs.fini`, and the image boots, so `init` decoded `root.cm` and started its children from version-3 files.
- The stripped `init` (`bin/init`, `init.image.elf`) went from 128,720 to 129,760 bytes (+1,040): the decoder's two bounds checks and the directory branch of `route()`, which `init` instantiates. `kernel.dev.obi` (rel) went from 1,005,960 to 1,006,104 bytes; `kernel.dev.dbg_1.obi` is 954,512 in both.

## Deviations from the design
- **The length check counts the Open, not the sentence.** The design says "a declared path, a `/` and the joined subdir together longer than `MAX_PATH_LENGTH`". The Open carries the declared path without its leading `/` (`export/data`), so the sentence counts one byte more than any Open needs. `open_path_fits()` counts what the Open carries: the path without its leading `/`, then `/` and the subdir when there is one. For `/export/data` a subdir of 1012 bytes fits and 1013 does not, where the design's wording would stop at 1011. Pinned by `ThePathAndTheSubdirTogetherMustFitOneOpen` and, for a path alone, `ADeclaredPathTooLongIsNotFoundAtTheProvider`. If the maintainer wants the stricter count it is one line in `route.cpp` and one number in two tests.
- **The brief's wording of the expose rule is the reverse of the rule.** "An expose that widens what the hop before it stated is AccessDenied at the expose" would deny a wider expose. The rule in the spec and in the design's test list is the other way round, as for every hop: walking from the use toward the source, stated rights may only grow, so an expose that states *less* than the hop before it is AccessDenied at the expose (`AnExposeNarrowerThanTheOfferBeforeItIsAccessDeniedAtTheChild`, `AnExposeIsHeldAgainstTheExposeBeforeIt`), and one that states more is routed (`AnExposeWiderThanTheOfferBeforeItIsRouted`). An expose wider than the *declaration* is denied at the declaration, which is where the "wider" in the brief probably came from (`AnExposeWiderThanTheDeclarationIsAccessDeniedAtTheProvider`).
- **The design's numbers and line references were right.** Sizes and offsets of both records, `VERSION` at `format.hpp:112` and `component_manifest.py:50`, the asserts at `format.hpp:227-228` and `:236-237`, the checks at `decl.cpp:82`, `:120`, `:158`, the views at `:310` and `:340`, `decl.hpp:39` and `:73`, the existing test at `route_test.cpp:1032`, and the sentences of `route.hpp` at `:259-260`, `:264-265`, `:267` and `:307-319` all matched the code at `a0e63b77`.
- "Images unchanged" holds for behaviour. The files are not byte-identical to the build before: the `.cm` files and the stripped `init` are bigger, as above.

## Found along the way
- The decoder's byte sweep (`EveryOneByteChangeStaysInBounds`) ran over fixtures that hold no directory, so nothing exercised a path or a subdir stated in a record. `directory_provider()` in `decl_test.cpp` (the provider's records patched into a directory's) is now swept too. No repair item.
- `route_test.cpp` cannot patch the root's own manifest, because `Tree` resolves the root in its constructor. `Tree` takes an optional hook that runs after the bytes are loaded and before the root resolves; the directory tests use it for `top.cm`'s offer. Step 2's compiled fixtures replace the patched tests, as the design's Staging table says, and the hook can go then.
- `log_route_error` in `realm.cpp` names the verbs "offer", "expose", "declaration" and "use". A directory denied at the declaration reads `routes nowhere: AccessDenied at <moniker> (declaration of "<name>")`, which is correct but does not say that the provider allows less than was asked. Step 4 touches that file and may want a better sentence; no repair item written.

## Not done
- The guide is not edited (the brief). It cites lines that moved; the list is below, for the later sync.
- The compiler's language (step 2), `Outgoing` (step 3), the root and `examples/files` (step 4).
- The design's own text: nothing in `DESIGN.md` was changed, including the one-byte wording above.

### Guide citations that moved
Every cite of `format.hpp`, `decl.hpp` and `decl.cpp` in `docs/guide/` still points at the same lines (there are none). The cites of `route.hpp` and `route.cpp` moved, because the spec comment grew and `route()` gained the two hops. Old lines are those at `a0e63b77`, new lines those of this commit.

| Guide line | What it cites | Old | New |
|---|---|---|---|
| `03-protocols.md:196` | the type check, in the spec | `route.hpp:291-305` | `route.hpp:318-333` (the paragraph is one line longer) |
| `03-protocols.md:198` and `:751` | "two protocols that share a type string are not told apart" | `route.hpp:352-354` | `route.hpp:388-390` |
| `04-tree-servers.md:467` | `Hops::apply`, the rule for hops | `route.cpp:95` | `route.cpp:109` |
| `05-testing-and-debugging.md:432` | cause 3: the built-in lookup, the offer lookup, the child and its expose, the program check | `route.hpp:412-415`, `:427-430`, `:444-451`, `:458-464` | `route.hpp:453-456`, `:468-471`, `:485-492`, `:505-511` |
| the same line | the error list in the spec | `route.hpp:307-317` | `route.hpp:335-357` (the list grew by four bullets) |
| `05-testing-and-debugging.md:435` | cause 6: the Rights bullet; the AccessDenied bullet | `route.hpp:271-282`, `:318` | `route.hpp:294-305`, `:349-354` |
| `05-testing-and-debugging.md:436` | cause 7: the protocol-only sentence of the type check; the built-in's type check; the type check at a provider | `route.hpp:301-305`, `:421-423`, `:465-467` | `route.hpp:328-333`, `:462-464`, `:512-517` (the directory branch follows, to `:529`) |

Not moved: every cite of `realm.cpp`, `component_manifest.gni`, `fsio`, `ioservice`.

## For step 2
- The Python `Capability` and `Expose` dataclasses already have `path`, `rights`, `subdir` and `rights`; the parser has to fill them and `encode()` writes them. `test_a_directory_capability_and_expose_encode_at_the_offsets_format_hpp_gives` builds such records by hand and can be kept as the encoder's own test.
- `test_the_language_is_not_opened_by_the_format_change` is the test that step 2 must change on purpose: it asserts the three refusals with the design's full spelling.
- A directory declaration that states no rights routes (it narrows nothing): the decoder and `route()` accept it, so decision 3 (a), "rights required", is the compiler's to enforce, as the design says.
- The root, when it gets `Route::path`, still has to answer InvArgs for a path that is empty or not absolute; `route()` checks only the length.
