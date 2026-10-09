# profdata changes

## A library that writes a program's coverage counters out as a raw profile llvm-profdata reads (2026-10-09)

**Files**
- `system/lib/profdata/include/profdata/profdata.hpp`, `src/profdata.cpp` — new; `profdata::RawProfile` sizes the profile, writes its fixed part and copies the counters in
- `system/lib/profdata/src/runtime.cpp` — new; the one symbol an instrumented program needs when it has no compiler runtime, as a target of its own
- `system/lib/profdata/test/` — new; a small instrumented example and four tests, one more that writes the dump to a file when asked
- `system/lib/profdata/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — from the library template, then edited
- `BUILD.gn` — the library's tests join `//:default`, so `ours-verify` runs them
- `.agents/tasks/cov/DESIGN.md`, `PLAN.md` — the step recorded

**Symptom**
A kernel built with source-based coverage has its counters in memory and no way to get them out as something `llvm-profdata` can read. The compiler's runtime does that, by writing a file when the program exits; a kernel has no such runtime and does not exit.

**Cause**
A raw profile is a header, the binary's build ID, the counters, and (unless the compiler was asked to keep them in the binary) a record and a name per function. The layout is not written down anywhere a program could read at run time: it is the table of macros in the compiler's own `InstrProfData.inc`, which expands into the header's struct, the order of its fields and every constant. The runtime exists to fill that layout in from the sections the compiler emitted, and to find those sections through names the linker defines (`__start_<section>` on ELF, `section$start$__DATA$<section>` on Mach-O).

Two properties of how the compiler lays the counters out decide the design. With `-mllvm -profile-correlate=binary`, which the kernel will use, only the counters section is loaded; the function records and names stay in the file (verified on the kernel target: `__llvm_prf_cnts` is `WAG`, the others are not `A`), so the profile is a header, a build ID and a flat array of 64-bit counters, and `llvm-profdata merge --binary-file=<elf>` finds the rest by build ID. And the counters are updated by plain stores from every CPU while the dump is being taken, so a dump is never one moment.

That second property is the concurrency argument the user wrote and had graded on 2026-10-08, carried over from the flag design to counters. The registry of section bounds is link-time constants, so there is nothing to lock. A counter only grows, so a copy taken while others run is a subset of the final counts and a superset of those at the start of the call, and a count it missed is in the next copy; what it can never show is a count that was not reached. The reader's side is a relaxed atomic load of each counter, which keeps the compiler from hoisting or tearing the read; the compiler's own stores are plain, which this accepts and says so where it copies.

The shape follows the repository's object rules. `RawProfile` establishes its invariant in the constructor (it computes the sizes, and asserts that they agree with the sections), where the original had a separate `Init()` that left the object unusable until called. Fixed text and live values are two methods because they have different lifetimes: `write_fixed()` once, `copy_live()` at the moment the caller chooses, as often as wanted.

**Change**
Taken from the original and kept: sizing the profile from the section bounds, the header built from the `INSTR_PROF_RAW_HEADER` table, writing the chunks in the order the format puts them, and copying the live regions out. Taken out, because nothing here uses it: merging two sets of counters, checking a profile against the running program, finding the build ID in a profile, and relocating the counters into another buffer (which needs `-runtime-counter-relocation`, and the compiler does not support that on macOS), the value-profiling stubs, and the PE-COFF branch. The ELF and Mach-O branches stay, ELF for the kernel and Mach-O so that the tests run on this Mac.

The runtime marker symbol is not in `RawProfile`'s target. The first version defined `__llvm_profile_runtime` in `profdata.cpp`, weak, so that a program with the compiler's runtime would not get a duplicate-symbol error. The coverage variant showed what that costs: the test's profile was missing (143 profiles merged for 144 tests, and this library's own row in the report read 0%). A static archive's member is pulled into a link only to define a symbol that is still undefined. The compiler's runtime puts this symbol and the code that writes the profile at exit in the same member, so a definition already in hand meant that member was never pulled, and the program wrote nothing. Weak was the wrong answer to a real problem: whichever program has a compiler runtime must not link a definition at all. So `:runtime` is a target of its own that the kernel will depend on, and the test depends on it only outside the coverage variant, where there is no compiler runtime. A program that links both gets a duplicate-symbol error, which is how a mistake in the wiring should show.

**Reference**
Fuchsia `src/lib/llvm-profdata/llvm-profdata.cc`: `LlvmProfdata::Init`, `DoFixedData` and `CopyLiveData` are where the sizes, the fixed writes and the copy come from. Its kernel user is Zircon `kernel/lib/instrumentation/profile.cc`, `LlvmProfdataVmo()`.

**Verification**
- Tests: four pass in the plain host build, a fifth skips unless `PROFDATA_TEST_DUMP` names a file. In the coverage variant all 144 host tests pass, 144 profiles merge, and this library reads 93.5% of its lines (200 of 214) and all its functions; what the tests do not reach is mostly the assertion failures.
- End to end, with the dump the library wrote for one call of the example's `classify`: `llvm-profdata merge` accepts the 472 bytes; `llvm-profdata show --all-functions` lists `_ZN16coverage_example8classifyEi`, three counters, function count 1; `llvm-cov show` gives 1 on the lines of `classify` that ran (7, 8, 11, 14, 15) and 0 on `return -1`, `return 0` and the whole of `never_called`; `llvm-cov report` counts 8 regions with 3 missed. That is what one call to `classify(7)` does.
- `clang-tidy` reports nothing in the library or its tests (it found one missing `[[nodiscard]]`, fixed), and `clang-format` is clean.
- The ELF branch compiles with the build's real kernel flags (`--target=x86_64-unknown-none-elf -mcmodel=kernel`) and the object holds the seven empty retained sections (`__llvm_prf_data` to `__llvm_prf_ucnts`). It was not linked or run.
- `ours-verify`: build ok, both images reach the end, host tests 144 pass (143 before). `shell: vmo_create_child checks FAILED` on both images, as in the previous entry, from the same uncommitted rewrite of `VmoChildOptions` in `vmo_dispatcher.hpp` and `syscall.hpp`; nothing in the kernel references this library.
- Not verified: the ELF branch at run time; the layout without function records (the example is not built with binary correlation, so `NumData == 0` is not exercised until the kernel is); single-byte counters; a non-empty bitmap or uniform-counters region (nothing here uses MC/DC).

**Deliberately not done**
- No consumer in the kernel, no variant for it, no way to get the dump out. Those are the next steps.
- The features listed under Change as taken out. Each can come back when something needs it.
- The library is `profdata`, not `llvm-profdata` as first said to the user: library directories here use one word or underscores, and the class is `RawProfile`, named for what it is.

**How it was found**
The missing profile showed as `merge 143 profiles` for `144 host tests` in `cov_host.py`'s own output, and as a row of 0% for the library whose tests had just passed. An assertion I wrote on alignment fired on the empty uniform-counters region, whose start the Mach-O linker places anywhere: an empty region has no alignment to check, so it is skipped before the assertion.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `__llvm_profile_runtime` in a target of its own, and what happens to a program that has the compiler's runtime and links it anyway?
- A kernel profile built with binary correlation holds only a header, a build ID and counters. Where do the function records and names come from when `llvm-profdata merge` reads it?
- `copy_live()` reads counters that other CPUs are incrementing. Why is that safe for what the counters are used for, and what does the relaxed atomic load buy over a plain copy?
