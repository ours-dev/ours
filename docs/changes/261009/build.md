# build changes

## A coverage variant for the host toolchain, picked by a `host = true` selector (2026-10-09)

**Files**
- `build/config/variants/BUILD.gn` — new `coverage` config: `-fprofile-instr-generate -fcoverage-mapping` for compiling, `-fprofile-instr-generate` for linking
- `build/config/ours/variants.gni` — `coverage` joins `ours_known_variants`, tagged `kernel-excluded`; the `select_variant` comment says what a `host = true` scope does
- `build/toolchain/ours/BUILD.gn` — the host toolchain reads the `host = true` selectors and takes the chosen variants' configs and tags
- `.agents/tasks/cov/DESIGN.md`, `PLAN.md` — the design corrected and the step recorded

**Symptom**
With `prebuilt/clang/llvm-23` now able to link a coverage-instrumented program, there was still no way to build the host tests that way. `select_variant = ["coverage"]` would have been accepted and done nothing for them, since a string selector is a non-host shortcut, and a scope with `host = true` was accepted by the parser and ignored.

**Cause**
Variants reach a toolchain only through `variant_toolchain_suite()`, and only the kernel and phys toolchains are suites (`build/switch/kernel_switchset.gni`, `phys_switchset.gni`). The host is a single `ours_toolchain("host")`, and the root `BUILD.gn` names each host test with `($host_toolchain)`, so no selector was ever consulted for it. There are two ways to give a single toolchain a variant: make it a suite (which would make the host tests ask for a variant toolchain by label, and the long list of `($host_toolchain)` entries would have to be written twice) or let it read the selectors itself. Reading the selectors is a few lines and keeps the one `select_variant` mechanism, so that is what this does.

The cost of that choice is the reason for the `host = true` rule. One toolchain has no per-target decision to make, so a selector that also carries `dir`, `label`, `name`, `output_name` or `target_type` cannot be honoured; the host branch asserts they are empty rather than ignore them silently. And everything the host toolchain builds is instrumented, the tools that run during the build as well as the tests. That is acceptable only in a build directory of its own in which just the tests are built, which is how the coverage run below was done.

Two details of the variant itself. The link needs `-fprofile-instr-generate`: through the clang driver that flag is what makes the driver add the profile runtime, and without it the object's undefined `__llvm_profile_runtime` has nothing to resolve to. (An earlier note in the design said the flag belonged in `cflags` only. That was true of `-fsanitize-coverage`, which asks for a sanitizer runtime the prebuilt does not have, and was wrongly carried over to the profile flags.) And the variant carries `kernel-excluded`, which `variant_toolchain_suite()` already honours for any suite tagged `kernel`: `select_variant = ["coverage"]` can therefore not pull the flags into a kernel toolchain, whose `ld.lld` link would not understand a driver flag and which has no runtime to write the profile.

**Change**
The host toolchain collects the configs and tags of the variants named by `host = true` selectors:

```gn
_host_variant_configs = []
_host_variant_tags = []
foreach(selector, ours_variant_selectors) {
  if (defined(selector.host) && selector.host && selector.variant != false) {
    assert(selector.target_type == [] && ... && selector.dir == [], "...")
    foreach(variant, ours_known_variants) {
      if (variant.name == selector.variant) {
        _host_variant_configs += variant.configs - _host_variant_configs
        _host_variant_tags += variant.tags - _host_variant_tags
      }
    }
  }
}

ours_toolchain("host") {
  ...
  configs = _host_variant_configs
  tags = _host_variant_tags
}
```

The first version kept a third list, `_host_variant_names`, to avoid listing a variant twice, and that version broke every build that selected no host variant. GN refuses a variable that is assigned and never read ("Assignment had no effect"), and in the default configuration the `foreach` body never runs, so the only read of that list, inside it, never happened. The coverage build did not show it, because there a selector existed and the read ran. Subtracting what is already in the list (`variant.configs - _host_variant_configs`) dedupes the same way and reads the list every time. A build directory is configured for coverage with `tools/set.py bringup.x86 --out out/cov-host --args 'select_variant=[{variant="coverage" host=true}]'`; GN scopes take no commas between statements.

**Reference**
None, this project's own design: the registry, the `select_variant` syntax and the `kernel-excluded` tag are this tree's own, and a variant is a plain config plus tags, as in `build/toolchain/variant.gni`. The flags are the pair the `coverage` variant in Zircon's `build/config/profile/BUILD.gn` uses (`-fprofile-instr-generate -fcoverage-mapping`), without its `-profile-correlate=binary`, which the host's standard runtime does not need.

**Verification**
- `gn gen` scenarios, each in a scratch directory: no selector, 977 targets, no file carrying the flag; `select_variant = ["coverage"]`, generates, still no file carrying it, in the host or in any kernel toolchain; `{variant="coverage" host=true}`, generates, and only `host/` carries the flag (169 ninja files; kernel, phys and user toolchain directories 0); `host=true` with a `dir`, the assertion with its message; an unknown variant name, the registry's existing error.
- In that coverage directory the sampler's `recorder` gtest built in 3.3 s (cached), ran (9 tests passed) and produced a profile. `llvm-cov report` against it, regions: `kernel/lib/sampler/src/dump.cpp` 89.47%, `recorder.cpp` 85.71%, `kernel_unwinder.cpp` 0% (that file belongs to the `stacks` test, not this one).
- `ours-verify` in the shared worktree: build ok, both images boot to the end, host tests 143 passed, but `shell: vmo_create_child checks FAILED` on dbg and on rel. The shared tree carries another session's uncommitted rewrite of `VmoChildOptions` (`kernel/object/include/ours/object/vmo_dispatcher.hpp`, `system/public/ours/syscall.hpp`). In a fresh worktree at HEAD with only these three build files added: build ok, dbg 35 passed 0 failed, rel 33 passed 0 failed, host tests 143 passed. So the failure is theirs, not this change's.
- Not verified: Linux hosts; the whole host test set under coverage (one test was run); GN formatting beyond what the hook did.

**Deliberately not done**
- No `--variant`-style switch in `tools/set.py` for host coverage; the scope goes through `--args` for now, and a wrapper belongs with the script that drives a coverage run.
- No group of host tests for the coverage directory, and no runner. Which tests, and how to find them without a second hand-kept list, is the next step.
- No kernel variant. The kernel will need its own config (no linker flag, `-profile-correlate=binary`) and the runtime port.
- The other session's `vmo_create_child` failure is not touched.

**How it was found**
The need came from reading `variant_toolchain_suite.gni` and the root `BUILD.gn`: the suite is only instantiated for the kernel and phys, and the host tests are named one by one. The `Assignment had no effect` came from a negative test with a plain string selector, which generates through the same code path as the default build. The `vmo_create_child` failure was told apart from this change by running the same verification in a worktree with none of the other session's edits.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a `host = true` selector not carry a `dir` or a `label`, and what does that mean for the build tools the host toolchain makes?
- Why did the first version of the host branch pass when a coverage selector was present and fail when none was?
- Why does the host variant put `-fprofile-instr-generate` in the link flags while the kernel's variant cannot?

## One command runs every host test under coverage and reports it per GN unit (2026-10-09)

**Files**
- `scripts/cov_host.py` — new; lists the host tests, builds and runs them, merges the profiles and prints the report
- `scripts/test_cov_host.py` — new; nine tests of the parts that need neither GN nor the LLVM tools
- `.agents/tasks/cov/PLAN.md`, `DESIGN.md` — the step recorded, and the limit below

**Symptom**
The coverage variant could build instrumented tests, but turning that into a number meant running some 143 programs by hand, each with a profile file of its own, merging the files, and asking `llvm-cov`, whose answer is one row per source file, several hundred of them, with googletest and the test sources mixed in.

**Cause**
Three questions had to be answered, and each has a wrong cheap answer.

Which tests. The root `BUILD.gn` names the host tests one by one with `($host_toolchain)`. A second list for coverage would drift from it, and the tests left off it are exactly the ones nobody would notice were missing. The script asks GN, by the rule the `ours-verify` skill uses for its host tests: the testonly executables of the host toolchain (`gn ls --testonly=true --type=executable`), restricted to those `//:default` reaches (`gn desc //:default deps --all`), and each one's file (`gn outputs`). The restriction matters: testonly only marks a target as a test, it does not say anything builds it, and the coverage directory would otherwise be asked to build tests that exist for no one.

Where each test's data goes. The profile runtime writes the raw profile when the process exits, to the name in `LLVM_PROFILE_FILE`. Two tests at once with one name would overwrite each other, so each gets its index and `%p` (the process id) in the name. The consequence of "written at exit" is that a test which crashes writes nothing: its coverage is missing, not wrong, and the script says that it failed. Profiles left from an earlier build do not match the new binaries, so they are removed before every run.

What to count. `llvm-cov` reports every file the coverage maps name: the test sources themselves, which are nearly always well covered and say nothing about the code under test, googletest, the standard library headers, generated files. They would fill the table and lift the total. They are filtered out by path (vendored code, the prebuilt toolchain, benchmarks, test directories, system headers, the build directory), and `--all` keeps the test sources. What remains is grouped by the nearest directory above the file that holds a `BUILD.gn`, because that is where this tree declares a library: `src/` and `include/` land in the same unit, and a `test/` that has a `BUILD.gn` of its own is a unit of its own. The per-file figures come from `llvm-cov export -summary-only`, which is JSON, rather than from the text of `llvm-cov report`.

**Change**
`scripts/cov_host.py` configures `out/cov-host` if it does not exist, refuses a directory that was configured without the host coverage selector rather than reconfiguring it, builds all the tests in one ninja call, runs them in parallel from the build directory, merges with `llvm-profdata merge -sparse`, and prints one row per unit plus a total. It also writes `coverage/report.txt`, `summary.json` and `all.profdata` beside the build, and an annotated HTML report with `--html`. It does not fetch the LLVM tools; it tells the user to run `fetch_llvm_coverage.py`, since that is a download of over a gigabyte.

The first lines of the real report:

```text
unit                               lines   regions  branches  functions  lines hit/all
build/toolchain/host-smoke-test   100.0%    100.0%         -     100.0%      3/3
kernel/arch/x86                    58.0%     90.9%     65.8%      58.3%    109/188
kernel/lib/sampler                 93.6%     94.1%     83.7%      97.7%    307/328
kernel/phys/lib/bootmem            19.5%     44.2%     33.6%      41.2%    116/594
system/lib/eftl                    15.0%     18.0%     11.1%      26.3%    417/2771
TOTAL                              72.6%     74.3%     63.3%      83.9%  13107/18050
```

**Reference**
None, this project's own design.

**Verification**
- On `out/cov-host`: 143 host tests from GN, built in 14.2 s, all 143 pass under instrumentation, 143 profiles merged. Lines 13107 of 18050 (72.6%), regions 74.3%, branches 63.3%, functions 83.9%. A second run gave the same totals, ran the tests in 0.3 s against 11.9 s the first time (I did not find out why the first was slower), and `--html` wrote `coverage/html/index.html`.
- The nine tests in `test_cov_host.py` pass, with the ten of `test_fetch_llvm_coverage.py`. They cover the grouping into units, the path resolution, the totals, the exclusion regex, the refusal of a wrongly configured directory, the test selection from canned GN answers, and one profile per test with a failure reported and a stale profile removed.
- Not verified: the numbers against an independent measure; Linux; a test that crashes under instrumentation (none did).

**Deliberately not done**
- The total is not the coverage of the tree. A unit appears in the table only if some host test compiles its sources, and code no test links never appears at all, though that is where coverage is lowest. So 72.6% is the coverage of the code the host tests build, and the kernel code that only runs in QEMU is absent. Counting the unreached files needs the list of a unit's sources from GN, or the kernel run's own coverage; both are later decisions, recorded in `DESIGN.md`.
- No threshold, no failing the run on low coverage, no comparison with an earlier run.
- No change to `ours-verify`. The test discovery is a copy of its rule, with a comment naming the source, rather than an import from a skill's script.

**How it was found**
A first run of the driver on the whole set was the check that the one-test result of the variant step held for all of them. The limit on the total came from reading what the table lacks: no `kernel/kernel` scheduler or most of `kernel/mm`, which no host test links.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the list of tests be intersected with what `//:default` reaches, and why does it come from GN and not from a file?
- Why does each test's profile name carry an index as well as `%p`, and what do you lose when a test crashes?
- The total says 72.6%. What does it leave out, and in which direction does that make it wrong?
