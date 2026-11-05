# kernel-test-migration changes

## Batch 1: the thirteen smallest console tests move to ktest (2026-11-05)

**Files**
- `kernel/commands/{vm_address,tick_fallback,vma_physical,vma_seal,pm_zone,wait_queue,vma_undo,vmo_pin,vma_phys,dpc,vmo_contiguous,tick_broadcast,irq_domain}_test.cpp` — moved to `kernel/tests/` with the same basenames.
- `kernel/commands/BUILD.gn` — the thirteen files leave the command library.
- `kernel/tests/BUILD.gn` — thirteen `kernel_test()` targets, and the shared header dependency list gains `//kernel/lib/user_copy:headers` and `//system/lib/logz4:headers`; `vma_physical` adds `//kernel/platform:headers` and `tick_fallback`/`tick_broadcast` add `//kernel/lib/arch:headers`.
- `docs/repairs/261105/kernel-test-framework.md` — the remaining-commands item now says 30, and which batch took which.

**Symptom**

After the three-test pilot, 43 of the kernel's tests were still console commands. Each carried its own `Tally`/`verdict()` and its own final `ok`/`FAILED` line, none of them appeared in `ktest list`, and the coverage run had to scrape their `GKTL_COMMAND` registrations to find them. This batch is the mechanical part of closing that gap: the thirteen smallest files, 135 to 298 lines each.

**Cause**

The migration was deliberately incremental: the pilot had to prove the interface on three tests of different shapes before a batch could be trusted. Batch 1 is the first test of whether the conversion rules scale past those three, which is why it is mostly mechanical and why its conversions were done in parallel and then reviewed as one diff.

**Change**

Every file follows the pilot's shape: one `KTEST(Suite, Body)` per original command body, `EXPECT_*` in the same order as the old `verdict()` calls, `ASSERT_*` for setup the rest of the body cannot run without, and the command's `Tally`, `verdict`, final lines and `GKTL_COMMAND` deleted. The file's header comment keeps what it checks and replaces "why a console command" with "why an in-kernel test".

The conversions that needed a judgement, and what was decided:

- `tick_fallback` and `tick_broadcast` had "cannot exercise this on this machine" paths that logged `ok -- skipped` so the command would pass. They are `KTEST_SKIP` now, which is what the interface's summary has a column for; in a `ktest all` run those tests now count as skipped rather than passed. This is the one verdict-shape change in the batch.
- `dpc` must not free its state while a Dpc may still be queued on it. The old command returned non-zero and leaked; the test keeps a local `ok` across the checks and calls `FAIL("a check failed; leaking the test's state")` at the end, so the leak-on-failure behaviour is explicit rather than implied by a return code.
- `wait_queue`'s suspend check was a helper that allocated; it is folded into the test body so the allocation can be an `ASSERT_NONNULL`, with the worker thread still only publishing state for the test thread to assert on.
- `vma_phys` and `irq_domain` have setup inside helpers where `ASSERT` cannot go (it would return from the helper, not the test); those paths use `FAIL` and return from the helper, exactly the old code's abandon-this-phase behaviour.
- `vma_seal`'s combined setup check became `ASSERT_TRUE`, which replaces the old `if (tally.failed) return 1`; the fatal creation log now prints `to_string(status)` instead of an integer.
- Diagnostic `log::info` lines that report measured values (frame addresses, lateness, free-frame counts, wait results) stayed, with their prefix changed from `<name>.test:` to the suite name; per-check pass lines and setup-error logs went. Six files no longer include `<logz4/log.hpp>` at all.
- The command-table paragraph ("the name has no underscore because gktl's table...") was removed from every file: a test name is not a command name any more, and the interface does not go through the encoder.

**Reference**

- The interface's design (`docs/design/261105/kernel-test-framework.md`) and its pilot entry; the three tests under `kernel/tests/` that landed in `9534e49b` are the pattern.
- Zircon's in-kernel framework has the same move: the old console-command test that prints its own verdict becomes a test function the suite runner reports on.
- GoogleTest: the equivalent editing is deleting a hand-rolled `main`-style reporter and putting `EXPECT_*`/`ASSERT_*` where the printer's calls were.

**Verification**

- `boot_check.py`: build ok; dbg PASS (46/5/0), rel PASS (44/8/0, 7/7 required); host-tests 173/0.
- `ktest list`: 23 tests (the ten before this batch plus the thirteen).
- `ktest all`: `SUMMARY ran=23 passed=22 failed=0 skipped=1`, verdict `ok, 23 ran, 22 passed, 1 skipped`, exit 0. The one skip is `Ktest.SkipIsReported`; the batch's own two skip paths (`TickFallback`'s oneshot refusal and `TickBroadcast`'s machine conditions) both took their normal path on this machine and passed.
- Each of the thirteen suites run alone through the host verdict, all `ok, 1 ran, 1 passed`: `Dpc`, `IrqDomain`, `PmZone`, `TickBroadcast`, `TickFallback`, `VmAddress`, `VmaPhys`, `VmaPhysical`, `VmaSeal`, `VmaUndo`, `VmoContiguous`, `VmoPin`, `WaitQueue`.
- The old command names are gone; `kernel/commands` holds 30 `*.test` commands now.
- `clang-format --check` clean on all thirteen; `clang-tidy -p out/bringup.x86` on all thirteen reports nothing in the migrated files; `gn check` OK on all thirteen targets and the aggregate; GN layout and docs lint clean.
- The slowest test is `VmoContiguous` at 136 ms (it counts the machine's free frames before and after); the batch adds about a second to an all-tests run in QEMU.
- Not verified: the two skip paths' *skip* branch (`TickFallback` refuses oneshot only on hardware that cannot arm it; `TickBroadcast` needs a machine without a usable group or with fewer CPUs), and `dpc`'s leak-on-failure branch. They are read-only judgements of the conversion.

**Deliberately not done**

- The remaining 30 commands, in `docs/repairs/261105/kernel-test-framework.md`; the next batch starts at `preempt_test.cpp` (350 lines) and stops before the files with an argument until that decision is made.
- No splitting of the converted bodies into smaller tests, and no new assertions: the batch's contract is that every check survived and nothing new is checked.
- No changes to the interface, the host verdict or the coverage script; all three already handle tests generically.

**How it was found**

None; this is a planned migration. The two conversions that needed a second look (`dpc`'s leak and `wait_queue`'s helper allocation) were identified by reading the original bodies before converting, and their decisions are described above.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. `tick_fallback` used to print `ok -- skipped` on a machine that cannot arm a oneshot tick; it is a `KTEST_SKIP` now. Which number in the host's summary changes, and why is a skip a better answer than a pass?
2. `dpc` keeps a local `ok` and calls `FAIL` at the end instead of using `ASSERT_*` on each check. What would change if it used `ASSERT_*` after the state had been allocated, and why would that be worse than the original?
3. Two conversions used `FAIL` inside a helper instead of `ASSERT_*`. Explain in terms of the macro's expansion what each would have done to the test.

## Run options for the tests that needed a console argument (2026-11-05)

**Files**
- `kernel/lib/ktest/include/ktest/options.hpp` — new: the `Option` type, `parse_option` and `find_option` (pure, host-testable), and the three accessors.
- `kernel/lib/ktest/src/runner.cpp` — parses `name=value` tokens after the selection into a fixed table, logs each as `ktest: OPTION name=value`, and implements `has_option` / `option` / `option_usize`.
- `kernel/lib/ktest/include/ktest/ktest.hpp` — includes the options header, so a test file needs one include for the declaration, the assertions and the options.
- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/test/BUILD.gn`, `kernel/lib/ktest/test/options_test.cpp` — the header joins `:headers`; two host tests cover parsing and lookup.
- `kernel/tests/ktest_test.cpp` — `Ktest.ReadsRunOptions`: the fallback path always, and the option path when the host passes `expected=<n>`.
- `scripts/kshell_run.py`, `scripts/test_kshell_run.py` — `ktest all rounds=256` still counts as a run of everything (so the list-count check applies), a filter run with options still does not; unit-tested.
- `kernel/lib/ktest/README.md`, `docs/design/261105/kernel-test-framework.md` — the grammar and the fallback rule.

**Symptom**

Four console commands took configuration from their command line: `interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `threadjoin.test stress N` and `timershare.test big`. A ktest test is a `void()` function with no arguments, and the runner's grammar had no room for a second kind of argument, so those four could not migrate as themselves. The user chose, on 2026-11-05, to give the interface a run-option mechanism rather than split each command into fixed-configuration tests (decision B of `.agents/tasks/kernel-test-migration/PLAN.md`).

**Cause**

Splitting was the cheaper path, but it would have changed what the four tests can do: the round count and the `big` flag are the only way to exercise the longer paths without editing the source, and the commands' help text is a contract someone may use in a script. A general mechanism is also more honest about the framework: options belong to the *run*, not to the test's identity, and a test that reads one can keep a default so `ktest all` stays deterministic. The alternative — a per-test argument mechanism keyed by test name — was rejected as a second registration channel for a need this small.

**Change**

The grammar gains one tail:

```text
ktest <selection> [name=value ...]
```

- A token is `name=value`, or `name` alone for a presence-only flag (`big`). The shell has already split on spaces, so the token is exactly what the parser sees; `parse_option` rejects an empty name and keeps everything after the first `=` as the value.
- The runner accepts at most `MAX_RUN_OPTIONS` (14) of them, resets the table per invocation, and logs each one before the first test: `ktest: OPTION rounds=256`. `ktest list` takes no options.
- A test reads them with `ktest::option(name, fallback)`, `ktest::option_usize(name, fallback)` and `ktest::has_option(name)`. The fallback is mandatory by construction: a test that does not get the option runs its ordinary configuration, which is what `ktest all` and a bare `ktest <filter>` depend on.
- `option_usize` reports a malformed value at error level and returns the fallback, rather than silently testing something else or aborting the test: the error line fails the run, and the test's own comparison then says what it actually checked.
- The value is a `std::string_view` into the console line: valid only while the run lasts, which the header says. Copying would need storage the framework does not otherwise have.

The accessors are not thread-local like the run state: options are read-only for the whole run and are read from the test body, on the test thread, so the same global publication the runner already does is enough. `Ktest.ReadsRunOptions` is the booted test of both paths, and the host check that `expected=41` fails proves the value actually reaches the test rather than a default.

**Reference**

- GoogleTest's configuration is process-global flags (`--gtest_filter`, `--gtest_repeat`), not per-test arguments; KUnit takes a `kunit.filter_glob` module parameter. This interface's options are the same idea kept at the smallest scope that needs it: one run.
- `docs/design/261105/kernel-test-framework.md` "Runner" for the grammar and "Run options" for the rule; `docs/repairs/261105/kernel-test-framework.md` names the four commands this unblocks.

**Verification**

- Host: `kernel/lib/ktest:ktest-test` now has 11 cases (two new option-parsing cases); `python3 scripts/test_kshell_run.py` 20 cases (the option-carrying forms included); all pass.
- `boot_check.py`: build ok; dbg PASS (46/5/0), rel PASS (44/8/0, 7/7 required); host-tests 173/0.
- Booted, three cases against the same test:
  - `ktest Ktest.ReadsRunOptions` — no `OPTION` line, `SUMMARY ran=1 passed=1`, exit 0 (the fallback path).
  - `... expected=42` — `ktest: OPTION expected=42`, `SUMMARY ran=1 passed=1`, exit 0 (the option is visible).
  - `... expected=41` — the failure line `ktest::option_usize("expected", usize{0}) == usize{42}: expected 41, actual 42`, `SUMMARY ran=1 passed=0 failed=1`, exit 1 (the value reached the test; the message also shows `usize` printing works).
- `ktest list`: 24 tests (batch 1's 23 plus `Ktest.ReadsRunOptions`).
- `clang-format --check`, `clang-tidy` on the changed files, `gn check` on the host header target and the framework test target: clean.
- Not measured: the cost of an option table per run (14 views, no allocation); it is noise next to a single test.

**Deliberately not done**

- No per-test options keyed by test name: a run's options are visible to every test it runs, and a test that does not read one is unaffected. Keying them would need the test name in the token and a second lookup rule for no clear need.
- No option types beyond string, unsigned decimal and presence flag; the four commands need exactly those.
- Not copied: the values live in the console line, as the header and the design say.
- The four commands themselves are still console commands; they migrate in batch 2 using this.

**How it was found**

The migration's decision card: the four argument-taking commands forced the question, and the user chose the mechanism (option B) on 2026-11-05.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. Why must a test that reads an option pass a fallback, and what would `ktest all` mean if it did not?
2. `option_usize` reports a malformed value at error level and returns the fallback instead of failing the test. Why is that better than either failing the test directly or silently using the fallback with no report?
3. The option values are views into the console line, not copies. What must a test not do with a value it read, and why is that acceptable here where the skip reason needed a copy?
