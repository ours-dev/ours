# kernel-test-framework changes

## Title: death tests for ktest — a record in its own section, and the host drives one fresh boot per test (2026-11-08)

**Files**

- `kernel/lib/ktest/include/ktest/ktest.hpp` — the `KTEST_DEATH(suite, name)` macro beside `KTEST`: the same expansion (static body, adapter, record) with the record placed in `.ktest.death`.
- `kernel/kernel.ld` — `.ktest.death` beside the other ktest registries in `.data.rel.ro`, `KEEP`'d, bounded by `__ktest_death_start`/`__ktest_death_end`.
- `kernel/lib/ktest/src/runner.cpp` — the section symbols; `list_death_tests` behind `ktest list`; the `death` selection (`run_death_tests`): one optional filter token, no options, `ktest: DEATH <name>` before each invoke, a returned body reported FAILED, a SUMMARY only on that survivor path.
- `kernel/tests/ktest_death_test.cpp`, `kernel/tests/BUILD.gn` — new: the booted smoke, `KTEST_DEATH(KtestDeath, PanicsOnDemand)`, whose body panics with a fixed message.
- `scripts/kshell_run.py` — `ktest death` as a command of its own: the death list read off the setup boot's `ktest list`, then a fresh image per selected test, `judge_ktest_death` reusing the crash report grammar, and `ends_machine` learning that `ktest death` ends the machine; `Console.close()` became idempotent so the loop can retire the setup console before main's `finally` closes it again.
- `scripts/test_kshell_run.py` — 15 new cases over the death verdict grammar (36 total).
- `docs/design/261105/kernel-test-framework.md`, `kernel/lib/ktest/README.md` — the death layer documented where the fixture and parameterized paragraphs are.

**Symptom**

None — this is feature batch 4. The design (`docs/design/261105/kernel-test-framework.md`) filed death tests under v1's non-goals: a test whose whole report is the kernel dying has no way to come back and no way to report, because a panic ends the machine and the framework cannot observe its own death from inside.

**Cause**

The v1 design deferred death tests for one reason that mattered to the shape it deferred: the only "catch and continue" the kernel has is the cooperative one inside the user-copy window, and a kernel-mode fault or a panic has no recovery hook — the machine ends, with whatever the interrupted code held left behind. Any in-kernel recovery (a `setjmp`/`longjmp` rewrite of the trap path, a killed test thread) risks leaving a lock held or a state half-updated, and a "successful" recovery would make every later test pass falsely. What was missing was not a recovery mechanism but a change of judge: the death test's verdict has to be read by something that survives the panic, which is the host.

**Change**

- The record side changed by one line of placement: `KTEST_DEATH` is the `KTEST` expansion with `KTEST_RECORD_SECTION(".ktest.death")` instead of `.hsien.tests`. The record type stays `hsien::TestRecord`, so the runtime is untouched for the fourth time (the suite hooks were the first second section, and the same reuse rule as `.hsien.tests` applies: records found by walking the range, `KEEP`'d because the walk is their only reference, read on demand so they stay in `.data.rel.ro` and out of the `.init` tail). The distinction between a live test and a death test rides on the section a record is in, not on a new field — which is exactly why the runtime keeps not knowing death tests exist.
- The live registry cannot reach them: `hsien::section_begin()`/`section_end()` bound `.hsien.tests`, and the runner's run loop reads only that range. `ktest all` and every filter run walk `.hsien.tests` alone; the SUMMARY of a normal run counts nothing from `.ktest.death`. `ktest list` prints the death registry after the live tests, one `ktest: DEATH Suite.Name` line per record — the host's handle on the list, and the reason the host never needs a second enumeration mechanism.
- The kernel side of a death run is the runner's walk with the verdict inverted: `ktest death [filter]` (a second selection word, one optional filter token, no options — `argc > 3` is a usage error) prints `ktest: DEATH <name>` before invoking each match and reports a body that *returns* as `FAILED (a death test must panic; it returned)`, ending at a SUMMARY whose `failed=` counts the survivors. A body that does its job never returns, so a death run that reaches its SUMMARY is by definition a failed one — the function returns 1. No environments, no suite hooks, no run plan: a body that ends in a panic leaves nothing behind to tear down, and the machine does not come back. The first panic also means the walk never reaches a second match, which is why the host, not the kernel, owns the sweep.
- The host's `ktest death` is the orchestrator the design sketch pointed at, built entirely from existing machinery: it reads the death list off the setup boot's `ktest list` (the `ktest: DEATH` lines), closes that console, and starts a fresh `Console` — a fresh QEMU — per selected test, where it types `ktest death <name>` and waits for either the panic report (the banner and the line after it, as a `crash` command's done-pattern) or the runner's own end (`ktest: SUMMARY`/`ktest: ERROR`). PASS is `judge_ktest_death`: the `ktest: DEATH <name>` marker present, the panic report behind it judged by the same grammar as a crash's (`judge_one_panic` — one banner, an error-level message, the ring replay between its markers), and no `kshell> ` prompt between the marker and the banner, which is what rules out a body that returned and some unrelated panic after it. The "nothing from the panicking CPU before the banner" check a crash demands is deliberately absent: the runner's own DEATH line comes from that CPU, on purpose. FAIL is the survivor path (the SUMMARY arrived), a hang (the silence NMI, as for any `ktest` command), or a boot that never reached the shell. An image with no death tests passes without a reboot; a filter that selects none of the listed tests fails without one. The filter itself is matched host-side (`ktest_filter_matches`, the runner's `:`-separated `-`-excludable glob grammar in the host's language), because the host types exact names per boot — passing the filter to the kernel on every boot would run the first match N times.
- `ends_machine()` learned that `ktest death` ends the machine, so it can only be the last command; the verdicts of the whole loop collapse into one verdict for the command. `Console.close()` gained a closed guard, since the loop retires the setup console while `main`'s `finally` still refers to it.

**Reference**

GoogleTest's death tests (`testing::Test::EXPECT_DEATH`, `vendor/googletest/googletest/include/gtest/gtest-death-test.h`) run the statement in a forked child and judge it from the parent by the child's exit status — the parent is the survivor that reads the verdict. This kernel has no fork and a panic ends the machine, so the roles split across the serial line instead of across a process boundary: the kernel's runner prints the marker and dies, and the host is the parent that reads the report and decides. Zircon's ktest has no death-test form at all (`zircon/system/ulib/ktest`, `zircon/system/utest/*`); its kernel-fault tests are run by the host harness rebooting the machine (`zircon/scripts/symbolize`-adjacent tooling, and the QEMU runner's per-test reset), which is the same "the host restarts, the kernel does not recover" shape this design sketched. Deliberately different: GoogleTest's `EXPECT_DEATH` matches the child's stderr against a regex per statement; here the whole body is the statement and the report grammar is the shared crash grammar, so no new matcher was introduced.

**Verification**

- Host: `test_kshell_run.py` 36 cases green — 15 new (classification, `ends_machine`, the filter grammar, the list read, the end pattern, six `judge_ktest_death` verdicts including the marker-less panic, the prompt-between case and the survivor), the old 21 untouched.
- ELF: `llvm-nm` on `out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours.with-tests` — `__ktest_death_start` 0x1b1898, `__ktest_death_end` 0x1b18b0: 0x18 bytes, exactly one `TestRecord`. The plain `vmours` (`kernel_x86.dbg.0`, the rel image's kernel) holds neither symbol: the section is absent there, the `.hsien.tests` rule.
- Boot: `boot_check.py` green (dbg 46/5/0, rel 44/8/0, host-tests 183/0).
- `ktest list`: `ok, 77 test(s)`, last line `ktest: DEATH KtestDeath.PanicsOnDemand` — the live count unchanged, the death heading after the live tests.
- `ktest all`: `ok, 77 ran, 75 passed, 0 failed, 2 skipped` — the two pre-existing skips; the death test counted nowhere.
- `ktest death`: the setup boot reads the list, then one fresh boot — `ktest: DEATH KtestDeath.PanicsOnDemand`, the other CPUs suspended, `OURS KERNEL PANIC`, `[... cpu0 error] ktest: death test reached its panic` — `ktest death KtestDeath.PanicsOnDemand: ok`, overall `ktest death: ok, 1 death test(s) panicked as expected, one boot each`. `ktest death KtestDeath.*`: same PASS. `ktest death Nope.*`: `FAIL, no death test matches "Nope.*" among the 1 listed`, no reboot.
- Failure path by hand: a temporary `KTEST_DEATH(KtestDeath, SurvivesOnDemand)` with an empty body — the runner printed the DEATH line, then `ktest: FAILED KtestDeath.SurvivesOnDemand (a death test must panic; it returned)` and `SUMMARY ran=1 passed=0 failed=1 skipped=0`; the host judged `FAIL, the boot is alive -- the death test did not panic` and the loop's overall verdict was `FAIL, 1 of 2 death test(s) did not die as expected`, with one boot per test. Removed before the commit.
- clang-tidy clean on every touched file (the pre-existing unrelated-header noise is unchanged); `format_cpp.py`/`format_gn.py` clean; `lint_docs.py` on the changed documents.

**Deliberately not done**

- `KTEST_EXPECT_PANIC("message")`, the sketch's fifth point: a form that pins the expected panic message per test. The host cannot know what message a record panics with while the record type stays `hsien::TestRecord` — a per-test message check needs a side table or a record extension, and neither earns its place while one smoke death test exists. The host judges the report's shape, and the message is the test author's recognizable text.
- Death tests in `ktest-test`, the host C++ suite: the only thing the macro does differently from `KTEST` is the section attribute, and the host toolchain has no ELF sections (`KTEST_RECORD_SECTION` drops it under `!__ELF__`); the booted run proves the section end to end.
- A death run carrying runner options or reading environments/suite hooks: the machine dies mid-run; there is nothing for a plan or a teardown to mean.
- Death tests on any image but the dbg one: the rel kernel links no test registry at all, and the host loop reads the dbg image's list.

**How it was found**

None.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. A death test and a live test carry byte-identical `hsien::TestRecord`s — what makes the runner run one and never the other, and why does that need no change to the runtime?
2. Why does `ktest death`'s SUMMARY only ever appear on the failure path, and what does the host read instead when the death test works?
3. The host types one exact name per boot instead of passing its filter to the kernel — what would go wrong with `ktest death <filter>` on every boot of the loop?
