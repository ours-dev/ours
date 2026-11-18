# ktest changes

## The runner becomes a Run object and the test records move into their classes (2026-11-18)

**Files**
- `kernel/lib/ktest/include/ktest/session.hpp` — new: `ktest::Run<Services>` owns one run's plan, shuffle order, summary and published test state; `list`, `execute` and `execute_death` are its methods, and logging and the clock come from the `Services` member
- `kernel/lib/ktest/src/runner.cpp` — the command keeps parsing and dispatch only and drives the static `Run<KernelServices> ACTIVE`; the run loop, the duplicate check and the shuffle log moved into `Run`; the second, never-used `gktl::Options` the command built on every call is gone
- `kernel/lib/ktest/include/ktest/run_stage.hpp` — moved from `src/run_stage.hpp` into the public headers, since `Run` (a header template) calls `run_hook`
- `kernel/lib/ktest/include/ktest/ktest.hpp`, `fixture.hpp`, `instantiate.hpp` — the record, the adapter and the per-test storage are static members of the generated test class (`Suite_Name_Test::record`, `::invoke`, `::storage`, `::records`) instead of namespace-scope variables named by token pasting
- `kernel/lib/ktest/include/ktest/environment.hpp`, `src/environment.cpp` — unchanged in the end: an unfinished rename of `SetUp`/`TearDown` to `set_up`/`tear_down` is reverted
- `kernel/lib/ktest/test/*.cpp`, `kernel/tests/ktest_fixture_test.cpp` — reach the records through the class members; every test environment's `SetUp`/`TearDown` carries `override`
- `kernel/lib/ktest/BUILD.gn` — lists the two new public headers

**Symptom**
None for the refactor itself: the format strings of the command's console lines are carried over unchanged, and the booted runs below match their expected summaries. The tree it started from was a half-finished copy of this change that compiled but failed 8 host tests: every `KtestEnvironment` walk-order case logged no stage at all, and three later cases saw a leftover framework error.

**Cause**
The procedural runner kept one run's state in namespace-scope variables (the plan, the shuffle buffer, the summary, a `RUNNING` flag) that free functions read and wrote, so nothing owned the invariant "only one run, and its state is valid only while it is entered". The test records were namespace-scope variables whose names were built by token pasting, so the record and the class it belonged to were tied together only by a naming convention.

The 8 failures came from the half-done rename. `Environment::SetUp` became `set_up`, but the test environments still declared `void SetUp()` without `override`. A non-`override` method with the old name compiles fine: it is just a new, unrelated method. The walk called the base's empty `set_up`, so no stage logged anything. The three cases that saw a leftover framework error read one global error counter that no case resets; they pass once the environments run again, so the error came from the broken environment cases, though which framework error it was was not traced.

**Change**
`Run<Services>` is the one owner of a run. `enter()`/`leave()` are the one-run-at-a-time guard, and `plan()` is what `ktest::option()` reads. The object stays in static storage because its shuffle buffer must not sit on the 16 KiB kernel stack the test bodies nest on. `Services` is a template parameter, so the run loop does not name the kernel logger or clock.

The rename was reverted instead of finished: `Fixture`'s stages are `SetUp`/`TearDown`, the GoogleTest authoring names the user chose on 2026-11-13, and an environment is the same idea one level up. Adding `override` to every test environment is what makes the compiler reject the next rename that misses a caller.

**Reference**
GoogleTest's `UnitTestImpl::RunAllTests` (`vendor/googletest/googletest/src/gtest.cc`) is the same shape: one object owns the run's state and walks the registry, and `testing::Environment` keeps `SetUp`/`TearDown` as virtual names.

**Verification**
`boot_check.py --out out/ktest-oop`: build ok, dbg PASS 46/5/0, rel PASS 44/8/0 (7/7 required), host-tests 185/0. On dbg, `ktest all` gave `ran=100 passed=84 failed=0 skipped=16`, and `ktest sel=all shuffle repeat=2 shard=0/2` gave `ran=100 passed=82 failed=0 skipped=18`. clang-tidy reports nothing new on the changed lines (the remaining four findings in `runner.cpp` are the linker symbols, unchanged since HEAD).

**Deliberately not done**
- `ktest sel=list` and `ktest sel=death` reject their own selection word. This is older than this change and recorded in `docs/repairs/261118/ktest.md`.
- `system/lib/hsien/include/hsien/environment.hpp` is still untracked and unused. Whether `Environment` belongs in hsien or in ktest is a design decision for the next step.
- The rest of the object-oriented refactor (the published state, the options, the environment registry, the command, and hsien's `Reporter` becoming an abstract base class) comes as separate steps of `.agents/tasks/ktest-hsien-oop/PLAN.md`.

**How it was found**
The host suite `ktest-test`, run against the half-finished tree before anything else was changed.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did `void SetUp()` in a class derived from `Environment` compile after the base renamed it, and what single keyword turns that into a compile error?
- Why must `Run<KernelServices>` live in static storage rather than in `ktest_command`'s frame?
- What ties a test's record to its class when the record is a static member, that a token-pasted namespace-scope name did not?

## `ktest sel=list` and `ktest sel=death` stop rejecting their own selection word (2026-11-18)

**Files**
- `kernel/lib/ktest/src/runner.cpp` — the list and death shape checks count the words besides `sel=`
- `docs/repairs/261118/ktest.md` — deleted, its only item is this fix

**Symptom**
On the dbg image, `ktest sel=list` printed `ktest: ERROR ktest sel=list takes no options` and listed nothing, and `ktest sel=death` printed its usage line and ran nothing. `ktest all` and filter runs were unaffected.

**Cause**
`invocation.words()` holds every word after the command name, the `sel=` word included. The list branch required that count to be 0 and the death branch 0 or 1 (with `filter=`), so the selection word alone broke the rule. The checks were written for the positional grammar, `ktest list`, where the selection was not one of the option words, and were not updated when `sel=` came in (d84981e3).

**Change**
After the parse, `other_words` is the word count minus one when the line carried `sel=`, and the two shape checks use it. The rejected alternative was to compare against 1 and 1-or-2: that breaks again for a bare `ktest`, which carries no `sel=` word and means `sel=all`.

**Reference**
None, this project's own command grammar.

**Verification**
`boot_check.py --out out/ktest-oop`: build ok, dbg PASS 46/5/0, rel PASS 44/8/0 (7/7 required), host-tests 185/0. On dbg, `ktest sel=list` passes with 100 tests listed plus `ktest: DEATH KtestDeath.PanicsOnDemand`, and a bare `ktest` gives `ran=100 passed=84 failed=0 skipped=16`. `ktest sel=death` passes: `KtestDeath.PanicsOnDemand` reached its panic, on a fresh boot of its own.

**Deliberately not done**
The `MAX_RUN_OPTIONS` bound before the parse counts the `sel=` word too, so a line with `sel=` and 14 test options (15 words, which the console allows) is refused although the plan's array holds 14. No test passes that many options. The bound moves into the command object of a later step of `.agents/tasks/ktest-hsien-oop/PLAN.md`.

**How it was found**
Running `ktest sel=list` by hand while verifying the previous change.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a bare `ktest` need the subtraction to be conditional?
- Why does `ktest sel=death filter=X` carry exactly one word besides the selection, and what does a second one mean?
- What would an object that owns the parsed command line answer instead of a raw word count?
