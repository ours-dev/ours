# build-toolchain outstanding repairs

## 1. variant_tags.gni keeps its upstream header and records none of its local changes

**Severity**
Low: attribution and documentation only; the tag list the build checks against is right.

**Symptom**
Latent, cannot be triggered today. `build/toolchain/variant_tags.gni` opens with another project's copyright line and has no PROVENANCE block, unlike every other ported file under `build/`, which carries this project's header and a PROVENANCE block that names the source file and lists what differs. A reader cannot tell that the file is not a verbatim copy: it drops three groups of tags (`fuchsia-excluded`/`fuchsia-only`, `uses-shlibs`, `cfi`), and since 2026-10-04 four of its comments (`custom-runtime`, `needs-compiler-abi`, `gcc`, `debugdata`) describe their tags in this tree's terms instead of the original's.

**Cause**
The file was copied when the GN build was added and edited later, but its header was never converted to the PROVENANCE form the other ported files use. Changing a file's copyright header is an attribution decision, which a comment-only sweep and its review do not make on their own.

**Change**
Give the file the header the other ported files have: this project's copyright line, then a PROVENANCE block naming the source file, its copyright and license, and the local changes (the dropped tags, each with the reason it is dropped, and the rewritten comments). Alternatively, if the file should stay a verbatim third-party copy, restore the dropped tags and the original comments and move it out of the swept tree; that option is not recommended, because the dropped tags select on things this build does not have.

**Reference**
The source is Fuchsia's `//build/toolchain/variant_tags.gni`. The header convention is this project's own: see `build/toolchain/toolchain_tags.gni` or `build/toolchain/variant.gni`.

**Acceptance**
`head -20 build/toolchain/variant_tags.gni` shows this project's copyright line and a PROVENANCE block that lists every difference that `diff` against the source file shows; `boot_check.py` builds and both images PASS.

## 2. Seven TODOs in the ported toolchain files are keyed to another project's bug tracker

**Severity**
Low: comments only; a question of how the 2026-10-04 comment rule applies.

**Symptom**
Latent, cannot be triggered today. `build/toolchain/hermetic_source_set.gni` (four TODOs), `build/toolchain/variant_tags.gni` (two) and `build/toolchain/verify_depfile.py` (one) write their TODOs as `TODO(https://fxbug.dev/<id>)`: the owner of the work item is a bug in another project's tracker, and the work it waits for (an LLD feature, one LLVM coverage backend for Clang and Rust, a crate-sources generator this tree does not have) is not tracked anywhere in this tree. The build sweep kept them as evidence links. The arch sweep of the same day rewrote the one such TODO it found, in `kernel/arch/x86/BUILD.gn`, as `TODO(ours)` with a statement of what is missing, so the tree now treats the same pattern in two ways.

**Cause**
The comment rule bans comparisons with another system and foreign names offered as comparisons; it does not say whether a link into another project's bug tracker is a citation, which is fine, or a foreign owner for this tree's own work, which is not. A `See <link>` after a sentence that states the fact is a citation; a `TODO(<link>)` hands the work item to the other project. The two sweeps answered the question differently.

**Change**
Recommended: rewrite each of the seven as `TODO(ours): <what is missing here, and what would let the line go>`, as in `kernel/arch/x86/BUILD.gn`, and keep the `See https://fxbug.dev/...` citations in `build/obi/resource.gni`, `build/toolchain/copy.py` and `build/toolchain/copy_command.sh`, whose sentences state the fact themselves. `verify_depfile.py` is a verbatim port whose PROVENANCE says only the shebang and header differ, so its PROVENANCE sentence has to be extended in the same change; its TODO guards an exemption for `//third_party/rust_crates:`, which does not exist here, so deleting the exemption is the alternative to rewording it, and that is a code change. The other option is to keep all of them as citations and say so in `CLAUDE.md`'s comment rule, so that the next sweep does not undo the arch group's rewrite.

**Reference**
None, this project's own convention: CLAUDE.md's comment rule of 2026-10-04, and the arch sweep's fixup on `mvp/integration`, "fix(arch): the sweep's leftovers: a foreign bug link, iretd, a repeated line", whose change record entry says "the retpoline config's TODO says what is missing".

**Acceptance**
`git grep -n 'TODO(https://fxbug' -- build` finds nothing, or CLAUDE.md states that such TODOs are allowed; `boot_check.py` builds and both images PASS.
