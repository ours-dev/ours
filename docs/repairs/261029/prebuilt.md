# prebuilt outstanding repairs

## 1. Nothing in the tree says how to fill `prebuilt/`, so a new developer cannot run the build or the boot check

**Severity**
Medium: no binary is wrong, but every build and boot instruction in the repository (the `ours-verify` skill, `docs/guide/README.md`) assumes `prebuilt/` already holds clang, gn, ninja, QEMU and GRUB, and a developer without them has no documented way to get them. It stops the outside developers the server guide is written for at the first command.

**Symptom**
Looked for on 2026-10-29, while writing `docs/guide/README.md`, and not found:

- Git ignores `prebuilt/` (`git check-ignore prebuilt` prints it), so a clone has none of it.
- `scripts/fetch_ccache.py` fetches ccache only; `scripts/fetch_llvm_coverage.py` fetches the coverage tools only.
- `scripts/activate.py` downloads the artifacts listed in a YAML manifest, whose default name is `prebuilt.yml`. The tree has no such file.
- `AGENTS.md` and `CLAUDE.md` say only that toolchains come from official upstream releases into `prebuilt/`.
- `.agents/tasks/build-toolchain/README.md` records the layout of 2026-08-17 (clang under `prebuilt/clang/llvm-22`, gcc, rust and ninja, some of them symlinks to another local tree), not how to produce it.

The guide's README therefore says that the tree does not yet say how to fill `prebuilt/`.

**Cause**
The directory is machine-local by design, the fetch scripts were written one tool at a time, and no manifest or document lists the whole set with its versions and sources. Not investigated beyond the files named above.

**Change**
Write the set down in one place, either a `prebuilt.yml` that `scripts/activate.py` reads or a section of a development document: each tool the build and boot scripts call (clang, gn, ninja, QEMU, GRUB, and ccache as an option), its version, the official release it comes from, and its checksum, with one command that fills the directory. Ruled out: building the dev container and installing into system directories, which `CLAUDE.md` forbids. When it lands, change the "Build and boot the dev image" section of `docs/guide/README.md`, which now says the tree does not tell.

**Reference**
None, this project's own tooling.

**Acceptance**
On a machine with only Git and Python 3, following the document gives a `prebuilt/` with which `python3 .agents/skills/ours-verify/scripts/boot_check.py` prints `PASS` for both images and the host tests.
