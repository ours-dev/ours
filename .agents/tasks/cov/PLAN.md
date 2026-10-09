# cov: plan for code coverage from LLVM source-based instrumentation

**Date**: 2026-10-08
**Status**: Proposed

## Request

Measure test coverage of host tests and the kernel, with a reusable tool that works across target architectures and host systems. The user chose option A of `DESIGN.md` on 2026-10-08: LLVM source-based coverage, a ported `llvm-profdata` writer, and the upstream `llvm-profdata` and `llvm-cov` for reports. Scope ends at host tests and the x86 kernel; user space, arm and riscv run-time checks are out. The user's rule for this task: no pinned versions anywhere.

## Decisions

See the Decisions table in `DESIGN.md`. One is still open: how the upstream tools reach `prebuilt/`.

## Steps

Later steps are written when each one is introduced to the user, not before.

1. Make the coverage tools available. Landed in `2f1e7bfa`. `scripts/fetch_llvm_coverage.py`, modelled on `scripts/fetch_ccache.py` but with nothing pinned:
   - It finds the compiler by running `--version` on each `prebuilt/clang/llvm-*/bin/clang` and taking the one that runs on this host, so no directory name or version is written in the script.
   - A tool is ready when it runs and reports exactly the compiler's version. If the tools are missing or report another release (the compiler was replaced), it fetches; where they already sit next to the real compiler that `bin/clang` points into (the Linux container may), it does nothing.
   - To fetch, it turns the compiler's version (`23.1.0-rc3`) into the release tag (`llvmorg-23.1.0-rc3`) and the host's asset name, downloads from the plain `github.com/.../releases/download/` URL (the GitHub API is rate limited without a token), and unpacks it.
   - It checks that the archive's own `bin/clang` carries the same source commit as the compiler in the tree, and keeps only `bin/llvm-cov`, `bin/llvm-profdata` and the profile runtime. Nothing under `prebuilt/` changes until that check and the whole download have succeeded.
   Why: no later step can be verified without these tools, and the macOS host cannot link a coverage-instrumented test without the profile runtime.

2. A coverage variant for the host toolchain. The `coverage` config and registry entry, and the host toolchain reading `host = true` selectors. Why now: the tools exist, but no host test could be built instrumented, and `select_variant` reached only the kernel and phys toolchains. Verified by `gn gen` in the default, string-selector and host-selector configurations and by a coverage run of one gtest. Lands as one commit.

3. A driver for the host tests. `scripts/cov_host.py`: ask GN for the host tests (the rule `ours-verify` uses, so there is no second list), build them in the coverage directory, run each with a profile file of its own, merge, and report per GN unit and in total with `llvm-cov export`. Why now: the variant builds one instrumented test; the point is a number for all of them. Verified by a run over all 143 tests and by unit tests of the parts that need no tools. Lands as one commit.

## Files

- `scripts/cov_host.py`, `scripts/test_cov_host.py`: new (step 3).
- `scripts/fetch_llvm_coverage.py`, `scripts/test_fetch_llvm_coverage.py`: new (step 1).
- `prebuilt/clang/llvm-23/bin/llvm-cov`, `llvm-profdata` and the profile runtime: fetched, not committed (`prebuilt/` is ignored).
- `build/config/variants/BUILD.gn`, `build/config/ours/variants.gni`, `build/toolchain/ours/BUILD.gn`: the variant and the host wiring (step 2).

## Test plan

- Step 1: the script on a clean `prebuilt/` installs the three files; a second run downloads nothing; an archive built from another commit, or whose tools report another release, changes nothing; a changed compiler makes the next run fetch again. The unit tests cover these against synthetic archives.
- Step 2: `gn gen` succeeds with no selector, with `select_variant = ["coverage"]` (no toolchain carries the flag) and with `[{variant="coverage" host=true}]` (only the host toolchain does); a `host = true` selector with a `dir` is refused; a gtest built in the coverage directory runs and `llvm-cov report` names the lines it reaches; `ours-verify` passes.
- Step 3: `python3 scripts/cov_host.py` builds and runs all host tests under coverage, every one passes, and the report has one row per GN unit and a total; a second run gives the same totals; the unit tests of the script pass.
