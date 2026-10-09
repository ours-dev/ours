# cov: code coverage for host tests and the kernel from LLVM source-based instrumentation

**Date**: 2026-10-08
**Status**: Proposed

## Problem

Nothing in the tree measures test coverage. The only instrumented variant is `ubsan-trap` (`build/config/ours/variants.gni`); the tags `coverage_clang`, `sancov` and `profile` are declared in `build/toolchain/variant_tags.gni` but no variant carries them, and `prebuilt/clang/llvm-23` has no `llvm-cov`, no `llvm-profdata` and no profile runtime. `docs/repairs/261005/prof.md` item 6 is one visible symptom: the kernel halves of the profiler have no automated guard, and nobody can say which lines the boot checks and host tests reach.

The tool has to be reusable: across target architectures (`kernel/arch/` has x86, arm and riscv), across host systems (macOS and the Linux dev container), and across environments (host gtest, kernel, later user space).

## Goals and non-goals

Goals:

- Line, region and branch coverage (MC/DC where the compiler supports it), reported by standard tools from a standard data format.
- One instrumentation scheme for host tests and the kernel, selected through the existing `select_variant` mechanism: per kernel entry target for the kernel, and for the host toolchain as a whole through a selector that says `host = true`, because the host is one toolchain and not a suite.
- Nothing in the kernel depends on a compiler-rt runtime.

Non-goals:

- Exact counts under SMP. Counters are lower bounds; the question asked is whether code was reached.
- Coverage of user space in the first slice. The user-space runtime has no `.init_array` (`kernel/lib/syscall/test/fault_probe/fault_probe.cpp`), so it needs a different way to find its counters.
- A QEMU-plugin collector and a sanitizer-coverage collector. Both were prototyped or studied and are kept as alternatives below.

## Design

Compile with `-fprofile-instr-generate -fcoverage-mapping -mllvm -profile-correlate=binary`. The compiler places a 64-bit counter on each source region, in static memory, and records which source range each counter belongs to in sections that stay in the binary (`__llvm_covmap`, `__llvm_covfun`, and with binary correlation `__llvm_covdata` and `__llvm_covnames`). Running code only increments counters; the runtime is not called.

A freestanding runtime turns the counters into an `llvm-profdata` raw profile on demand. It is `system/lib/profdata`, `profdata::RawProfile`, a trimmed port of Fuchsia's `src/lib/llvm-profdata` (written for kernels and boot loaders, does no I/O): the constructor sizes the dump, `write_fixed()` writes the header and build ID, `copy_live()` copies the counters out of their link-time locations with relaxed atomic loads. What the kernel's own use of it needs beyond that goes in `kernel/lib/k*`, the split `layering.md` calls for. The marker symbol that instrumented code refers to is the library's separate `:runtime` target, linked by a program that has no compiler runtime and never by one that has.

The kernel hands the dump to the host through a kshell command over the serial line. A thin Python script on the host rebuilds the `.profraw`, then runs `llvm-profdata merge` and `llvm-cov report` or `show` against the ELF. Host gtests link the upstream profile runtime instead and write `.profraw` files themselves.

Counterparts: Zircon `kernel/lib/instrumentation/profile.cc` (the runtime hookup), `src/lib/llvm-profdata` (the writer), `build/config/profile/BUILD.gn` (the flags). Linux has no source-based coverage in mainline (this checkout, v7.x, has no `kernel/pgo`); `kernel/gcov` exports the gcov format through debugfs and takes per-directory opt-in (`GCOV_PROFILE := y`).

Concurrency. The registry of counter ranges is written once at start-up by the boot CPU and is read-only afterwards, so it needs no lock; the start-up completing before any reader runs is the publication point. A counter only ever grows, and "non-zero" means reached, so concurrent increments may lose counts but cannot lose reachability, and a dump taken while other CPUs run is a subset of the final set and a superset of the set when the dump began. The reader should still load counters with a relaxed atomic load: the compiler's stores are plain stores, which we accept and say so in the code. This argument was written by the user for the earlier boolean-flag design and graded on 2026-10-08; it carries over to counters, except that counts become lower bounds. A module loaded after start-up would break the first half and need a publication step.

Verified so far (2026-10-08, `prebuilt/clang/llvm-23`, freestanding targets): `-profile-correlate=binary` is accepted; the only undefined symbol in an instrumented object is `__llvm_profile_runtime`. On macOS the prebuilt clang had no `libclang_rt.profile_osx.a`, so a coverage-instrumented program did not link; `scripts/fetch_llvm_coverage.py` now installs it. With the runtime in place the link needs `-fprofile-instr-generate` as well: through the driver it is what makes the driver add the runtime. (An earlier note here said the flag belongs in `cflags` only; that came from `-fsanitize-coverage`, which asks for a sanitizer runtime the prebuilt does not have, and does not apply to the profile flags.) The kernel links with `ld.lld` directly, so its variant will differ: no linker flag, and `-profile-correlate=binary`.

Host coverage is a variant of the host toolchain as a whole (2026-10-09). Every host binary the build makes is instrumented, the tools that run during the build as well as the tests, so it is meant for a build directory of its own, configured with `select_variant = [{variant="coverage" host=true}]`, in which only the host tests are built. The variant carries the `kernel-excluded` tag, so the kernel toolchain suites never instantiate it.

## Alternatives considered

- Own sanitizer-coverage runtime (`inline-bool-flag,pc-table`) with a Python DWARF reader and report tool. Works on x86_64, aarch64 and riscv64 freestanding targets, but line coverage is only approximate (a block's range runs to the next table entry), there is no branch or region coverage, and it rebuilds what Zircon already has.
- Zircon's sanitizer coverage (`trace-pc-guard`, `kernel/lib/instrumentation/sancov.cc`). PC level only, one function call per block, and start-up needs special handling for hits before the buffers exist.
- gcov format through Clang's `--coverage`, as Linux does (`kernel/gcov/clang.c`). Feasible by capturing the `llvm_gcda_*` callbacks in memory, but gives only line and arc coverage.
- A QEMU TCG plugin recording executed instruction addresses. Prototyped on the x86 dbg image: 325243 addresses, 49978 distinct in the kernel, 5067 of 10544 lines under `kernel/` reached. Line-exact without touching the kernel, but QEMU-only and needs a C plugin built per host. May return later as a second collector for code the compiler cannot instrument, such as the phys boot path.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Collector | A: LLVM source-based with a ported writer; B: own sanitizer-coverage runtime; C: both | A | None | A, 2026-10-08 |
| Lock for the registry and the counters | A lock; no lock | No lock | No lock: the registry is written only at start-up and read-only after; setting a flag twice has the same effect, and we ask whether a path was reached, not how often | No lock, 2026-10-08 |
| Port Fuchsia's `llvm-profdata` library | Port; write our own | Port | None | Port, 2026-10-08 (part of A) |
| Which parts of the port to keep | All of it; what the kernel and the host tests use | What is used: sizing, the fixed part, the live copy. Merge, verify, build-ID lookup, counter relocation, value-profiling stubs and PE-COFF come back when something needs them | None | What is used, 2026-10-09 |
| LLVM tool version | Pin a release tag in the script; read it from the compiler in the tree | Read it from the compiler: `llvm-cov` must match the compiler that wrote the coverage maps, and the two hosts already differ (llvm-23 on macOS, llvm-22 in the container) | None | Read it from the compiler, 2026-10-08; the user's rule is no pinned versions, so a compiler swap never needs a script edit |
| How the upstream tools reach `prebuilt/` | Stream the 1.5 GB official archive and keep three files; copy from a local Homebrew install | Official archive | None | |

## Testing and verification

- Fetch script: a second run changes nothing; an archive whose compiler carries a different source commit than the one in the tree leaves `prebuilt/` untouched; a changed compiler makes the next run fetch again; `llvm-cov --version` and `llvm-profdata --version` run.
- Host: a gtest built with the coverage variant writes a `.profraw`, and `llvm-cov report` names the lines the test reaches.
- Kernel: the dbg image boots, the dump command prints a profile, and the host script turns it into a report.
- Cannot be verified yet: arm and riscv at run time (no bootable image), and user space.

## Open questions

- The host report counts only code that some host test compiles. A unit that no test links does not appear and cannot lower the total, although it is the least covered code; the total is therefore the coverage of the code the host tests build, not of the tree (first full run, 2026-10-09: 13107 of 18050 lines, 72.6%). The kernel report does not have this limit, because the counters of code that never ran are in the profile as zeros: its denominator is the whole kernel (2026-10-09: 26894 of 39491 lines, 68.1%).
- Whether the dev container already has `llvm-cov`, `llvm-profdata` and the profile runtime: `docker/dev.dockerfile` installs the latest stable LLVM from apt.llvm.org, and `prebuilt/clang/llvm-22/bin` is symlinks into `/usr/lib/llvm-22`, but the symlinked set does not include these tools and this cannot be checked from macOS.
- The kernel report covers the dbg image's kernel only. The phys modules (boot shim, physload) and user space are not instrumented; the first needs its own runtime and a way out before any console exists.
- Whether the kernel report should also be summed with the host report per GN unit, so that a unit tested on the host and in the kernel shows one number.
