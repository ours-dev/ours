# cov: plan for code coverage from LLVM source-based instrumentation

**Date**: 2026-10-08
**Status**: Proposed

## Request

Measure test coverage of host tests and the kernel, with a reusable tool that works across target architectures and host systems. The user chose option A of `DESIGN.md` on 2026-10-08: LLVM source-based coverage, a ported `llvm-profdata` writer, and the upstream `llvm-profdata` and `llvm-cov` for reports. Scope ends at host tests and the x86 kernel; user space, arm and riscv run-time checks are out. The user's rule for this task: no pinned versions anywhere.

## Decisions

See the Decisions table in `DESIGN.md`. One is still open: how the upstream tools reach `prebuilt/`.

## Steps

Later steps are written when each one is introduced to the user, not before.

1. Make the coverage tools available. Add `scripts/fetch_llvm_coverage.py`, modelled on `scripts/fetch_ccache.py` but with nothing pinned:
   - It finds the compiler by running `--version` on each `prebuilt/clang/llvm-*/bin/clang` and taking the one that runs on this host, so no directory name or version is written in the script.
   - If that directory already resolves `llvm-cov`, `llvm-profdata` and the profile runtime (the Linux container may), it does nothing.
   - Otherwise it turns the compiler's version (`23.1.0-rc3`) into the release tag (`llvmorg-23.1.0-rc3`) and the host's asset name, downloads from the plain `github.com/.../releases/download/` URL (the GitHub API is rate limited without a token), and streams the archive through the decompressor.
   - While streaming it checks that the archive's own `bin/clang` carries the same source commit as the compiler in the tree, and keeps only `bin/llvm-cov`, `bin/llvm-profdata` and the profile runtime. Nothing under `prebuilt/` changes until that check and the whole download have succeeded.
   - It records the tag and commit in a stamp, so a later run that finds a different compiler fetches again by itself.
   Why now: no later step can be verified without these tools, and the macOS host cannot link a coverage-instrumented test without the profile runtime. Verified by `llvm-cov --version` and `llvm-profdata --version`, and by a second run that downloads nothing. Lands as one commit.

## Files

- `scripts/fetch_llvm_coverage.py`: new.
- `prebuilt/clang/llvm-23/bin/llvm-cov`, `llvm-profdata` and the profile runtime: fetched, not committed (`prebuilt/` is ignored).

## Test plan

- Run the script on a clean `prebuilt/`: expect the three files and a stamp naming the tag and commit.
- Run it again: expect "already current" and no download.
- Point it at a compiler whose commit differs from the archive's (a scratch copy of the stamp and a faked version string): expect a failure and no change under `prebuilt/`.
- Change the recorded compiler in the stamp: expect the next run to fetch again.
