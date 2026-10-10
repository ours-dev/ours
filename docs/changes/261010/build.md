# build changes

## Host tests declare themselves: host_test() and a generated host_tests.json (2026-10-10)

**Files**
- `build/cpp/host_test.gni` — new; `host_test()`, an `executable()` that is always `testonly` and declares itself in GN metadata
- `BUILD.gn` — the root build file collects the declarations into `$root_build_dir/host_tests.json`
- `kernel/lib/sampler/test/BUILD.gn`, `system/lib/profdata/test/BUILD.gn` — four tests moved to `host_test()`, as a trial
- `scripts/cov_host.py`, `scripts/test_cov_host.py` — read the declared tests first, and find the rest by the old rule
- `.agents/tasks/cov/PLAN.md`, `DESIGN.md` — the step recorded

**Symptom**
The host tests had no list of their own. The scripts that run them worked the list out by asking GN three questions: which executables are `testonly`, which of those does `//:default` build, and where is each one's file; and then left out by hand what is not a test (`//benches/`). The rule was written twice, in `boot_check.py` of the `ours-verify` skill and in `cov_host.py`, which says so in a comment.

**Cause**
`testonly` is not a mark that a target is a test. It is a rule about who may depend on whom: only a `testonly` target may depend on a `testonly` one, so that test code cannot get into a shipped artifact. Everything that obeys it carries it: the tests, but also benchmarks, a helper a test uses, and the instrumented example `profdata`'s tests dump. So every list built from it needs a second rule that separates the tests from the rest, and each such rule is a guess (a path prefix, a toolchain suffix) that nothing checks. The honest way to know what is a test is for the test to say so. This tree's rule for lists of build products is to generate them from GN metadata, not to keep them by hand, and `obi.gni`, `product.gni` and the distribution manifests already do it with `data_keys`.

**Change**
`host_test()` is an `executable()` whose metadata says what it is:

```gn
template("host_test") {
  _label = get_label_info(":$target_name", "label_with_toolchain")
  _binary = rebase_path("$target_out_dir/$target_name", root_build_dir)
  executable(target_name) {
    forward_variables_from(invoker, "*")
    testonly = true
    metadata = { ours_host_tests = [ { label = _label  binary = _binary } ] }
  }
}
```

and the root build file walks the metadata from `//:default`:

```gn
generated_file("host_tests") {
  testonly = true
  deps = [ ":default" ]
  data_keys = [ "ours_host_tests" ]
  outputs = [ "$root_build_dir/host_tests.json" ]
  output_conversion = "json"
}
```

GN writes the file when it generates, before anything is built. The walk starts at `//:default`, so a test that nothing there depends on is not listed; it would not be built either. `cov_host.py` reads the file and takes the tests it lists first. The tests not yet moved are still found by the old rule, and the two are joined, a test that is in both counted once, because a script that took the file alone would silently drop every test not yet moved. Its output says how many came each way (`4 declared with host_test(), 140 found by the old rule`), so the move can be followed, and the old rule can be deleted when the second number reaches zero.

The template wraps a built-in target type, which `cpp_interface.gni` says this tree does not do, because a transparent wrapper adds indirection without value. That comment names metadata as a reason a wrapper may be added, and this wrapper is not transparent. `host_test()` asserts that the caller sets neither `testonly` nor `metadata`.

Rejected: leaving the list to be guessed (the cost is above); moving every test at once (35 build files, with no way to see the move go wrong in one step); having `cov_host.py` take only the file (it would drop the tests not yet moved).

**Reference**
None, this project's own design. The shape of the metadata walk is `build/obi/obi.gni` and `build/product/product.gni`. Fuchsia's `src/lib/llvm-profdata/BUILD.gn` declares its tests with a `test()` template from `//build/test.gni`; that template's body was not read.

**Verification**
- `gn gen` of the default directory and of the coverage directory; `host_tests.json` lists the four moved tests, and for the five that were moved at one point the `binary` of each was equal to what `gn outputs` gave.
- `cov_host.py` over the coverage directory: 144 host tests, 4 declared and 140 found by the old rule, all 144 pass, 144 profiles merged, lines 13,307 of 18,264 (72.9%), the same as before the change.
- The 12 tests of `test_cov_host.py` pass, with the other two script suites. The new ones: without a file the old rule alone; declared tests first and a test in both counted once; an entry in another toolchain is not a host test; declared tests alone are enough; no tests at all is an error.
- `ours-verify` in a clean worktree: build ok, dbg 35 passed 0 failed, rel 33 passed 0 failed, host tests 144 passed.
- Not verified: that a test left out of the metadata would be noticed by anything (nothing yet compares the two sets in the other direction).

**Deliberately not done**
- About thirty build files are not moved. Two directories cannot be moved from here at all: `system/lib/concurrent` and `system/lib/lockdep` are git submodules, not initialised, whose directories hold files this repository does not track, so a change there is not a change to this repository. Their tests stay on the old rule, and the old rule cannot be deleted until that is dealt with.
- `ours-verify`'s own copy of the rule is not touched: it belongs to a skill other sessions run, and changing it is agreed with them first. When it reads the file, the duplication ends.
- No per-test properties (timeout, arguments, environment, whether it counts for coverage) yet. The metadata entry is where they would go.
- Benchmarks are not given a template. They are not `host_test()`, so they are not listed, which is the point.

**How it was found**
The user asked whether the test targets could use one template. Counting how the 144 tests are declared today gave 35 build files with `executable()` and `testonly`, and no C++ template. A first try moved `system/lib/concurrent`'s test as well; the formatter answered `skipped: inside a submodule`, and `git ls-files -s` showed it as a gitlink. That edit was undone before anything was committed.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `testonly` is on benchmarks and test helpers as well as tests. What does it actually forbid, and why does that make it a poor way to find the tests?
- Why does `cov_host.py` join the declared tests with the ones the old rule finds, and not take the file alone?
- The walk that builds `host_tests.json` starts at `//:default`. What does that mean for a host test nothing there depends on?
