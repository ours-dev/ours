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
