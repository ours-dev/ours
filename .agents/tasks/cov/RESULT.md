# cov: LLVM source-based coverage for the host tests and the x86 kernel, reported per GN unit

**Date**: 2026-10-14

## Delivered

- `2f1e7bfa` `scripts/fetch_llvm_coverage.py`: finds the compiler in `prebuilt/`, derives the LLVM release from its version, and installs `llvm-cov`, `llvm-profdata` and the profile runtime for exactly that compiler. Nothing is pinned.
- `e117defb` The `coverage` variant for the host toolchain, chosen by a `host = true` selector in `select_variant`.
- `c97ab270` `scripts/cov_host.py`: builds every host test in a coverage directory, runs each with a profile file of its own, merges, and reports per GN unit and in total.
- `f2719f03` `system/lib/profdata`: `profdata::RawProfile` and the `:runtime` target, a trimmed port of Fuchsia's writer, with host tests.
- `b5e4e4a1` The kernel: the `coverage-kernel` variant, the coverage sections in `kernel/kernel.ld`, the `cov dump` kshell command, and `scripts/cov_kernel.py`, which boots the dbg image, runs every `*.test` command, dumps after each and merges.
- `4a7d48db`, `8ab9400b` A `host_test()` template that moved 143 tests; withdrawn in `43bf72e7`, which leaves `scripts/host_tests.py` as the one rule for finding host tests, used by `cov_host.py` and by the `ours-verify` skill's `boot_check.py`.
- `aac7503b`, `4a4ccf84` `system/lib/concurrent` and `system/lib/lockdep` made real submodules (found on the way, see below).

Commands: `python3 scripts/fetch_llvm_coverage.py`, `python3 scripts/cov_host.py`, `python3 scripts/cov_kernel.py`.

## Verification

- Host, 2026-10-14: 144 tests built and run under coverage, 144 pass, 144 profiles merged; 13,326 of 18,264 lines, 73.0%. That is the coverage of the code the host tests build, not of the tree.
- Kernel, in a clean worktree on 2026-10-09: all 17 kernel tests pass instrumented; 26,894 of 39,491 lines, 68.1%, of the whole kernel (the counters of code that never ran are zeros in the profile, so the denominator is complete). The decoder was also run over a 105 MB serial log.
- Unit tests of the scripts: `test_host_tests.py` (4), `test_cov_host.py`, `test_cov_kernel.py`, `test_fetch_llvm_coverage.py` (10); all pass.
- `ours-verify` after the submodule pins (2026-10-14, shared tree): dbg 39 passed, 4 skipped, 0 failed; rel 37 passed, 8 skipped, 0 failed; 145 host tests passed (a later tree than the 144 of the coverage run, which other sessions add tests to).
- Not measured: the dev container (Linux), arm and riscv at run time, user space, the phys modules.

## Deviations from the design

- The kernel side is one file, `kernel/commands/cov.cpp`. The design's "what the kernel's own use needs goes in `kernel/lib/k*`" was not done.
- Three choices were made at the user's "just do it" without a separate question: the transport (framed text lines on the console, each carrying its offset and a checksum, because the console is shared), the build ID read from the image's own ELF note, and the `cov` command being part of every dbg kernel (it answers "built without coverage instrumentation" when it is not).
- The `host_test()` template of the plan's step 6 was built and then withdrawn; see `docs/design/261014/cov.md`, Decisions.
- A weak definition of `__llvm_profile_runtime` in the profile writer's library kept the compiler's profile runtime out of the link in the coverage variant, and one profile of the 144 was missing. The marker is therefore a strong symbol in a separate `:runtime` target that only a program without a compiler runtime links; the design text says so.

## Found along the way

- The two submodules the kernel builds from held code that no repository recorded; fixed, with the record in `docs/changes/261014/build.md`.
- `vendor/cxxopt`, `vendor/googletest` and `vendor/nlohmann_json` are in the same state, and are unfixed: `docs/repairs/261014/build.md`.
- The kernel tests `interruptline`, `port` and `semaphore` failed or locked up during one coverage run. The cause was another session's uncommitted edit of `kernel/kernel/task/thread.cpp`, not the coverage build and not a committed change, so there is nothing to repair in the repository.
- The coverage output sections have to be named in `kernel/kernel.ld` with the compiler's own names (`__llvm_prf_cnts` and the rest): `llvm-profdata merge --binary-file` looks sections up by those names, and a section named with a leading dot is not found.
- `scripts/new_worktree.py` still copies the submodule directories by hand, which is no longer needed for `concurrent` and `lockdep`. Not changed.

## Not done

- Coverage of user space, arm and riscv at run time, and the phys modules (they need their own runtime and a way out before any console exists): `docs/design/261014/cov.md`, Open questions.
- The host report's denominator: a unit that no host test links does not appear, so it cannot lower the total: same place.
- One report for both runs, summing the host and kernel numbers per GN unit: same place.
- Whether the dev container already has the three tools: same place.
- Using the kernel report to choose the next core tests (`owned_wait_queue`, `page_queues`), offered to the user and not started; recorded nowhere else.
