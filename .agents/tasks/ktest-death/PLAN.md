# ktest-death: death tests, one fresh boot per test

**Date**: 2026-11-08
**Status**: Proposed

## Request

Feature batch 4 of the kernel test framework (step 4 of
`.agents/tasks/ktest-features/PLAN.md`, on the branch that carries it; design:
`docs/design/261105/kernel-test-framework.md`, "Death tests and kernel
faults", which v1 filed death tests under non-goals). `KTEST_DEATH(Suite,
Name)` records a `hsien::TestRecord` in a new `.ktest.death` linker section
(`__ktest_death_start`/`__ktest_death_end`, KEEP'd), so the live registry
never runs one and a normal run's SUMMARY does not count it. `ktest list`
prints the death registry under `ktest: DEATH Suite.Name` lines after the
live tests. `ktest death [filter]` runs the matching death tests — a body is
expected to panic the kernel, so the first one ends the machine and the host
gives each test a fresh boot: `scripts/kshell_run.py`'s `ktest death` reads
the list, reboots per test, types the one name, and judges PASS when the
panic report follows the runner's `ktest: DEATH` line, FAIL when the command
ends at its SUMMARY (the body returned) or the console hangs. No kernel
recovery: the design's host-orchestrated shape, unchanged.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

None taken outside the request; the judgment calls are recorded in the change
entry (`docs/changes/261108/kernel-test-framework.md`).

## Steps

1. `kernel/lib/ktest/include/ktest/ktest.hpp`: the `KTEST_DEATH` macro beside
   `KTEST` — same expansion, record placed by `KTEST_RECORD_SECTION(".ktest.death")`.
2. `kernel/kernel.ld`: `.ktest.death` in `.data.rel.ro` beside the other
   ktest registries, `KEEP`'d, bounded by `__ktest_death_start`/`__ktest_death_end`.
3. `kernel/lib/ktest/src/runner.cpp`: the section symbols, `list_death_tests`
   behind `ktest list`, and the `death` selection — `run_death_tests(filter)`
   walks the section, prints `ktest: DEATH <name>` before each invoke, and
   reports a returned body FAILED; no environments, suite hooks or plan.
4. `kernel/tests/ktest_death_test.cpp` + `kernel/tests/BUILD.gn`: the booted
   smoke, one `KTEST_DEATH(KtestDeath, PanicsOnDemand)` whose body calls
   `ours::panic("ktest: death test reached its panic")`.
5. `scripts/kshell_run.py`: `ktest_deaths`/`ktest_death_filter` (the host's
   copy of the filter grammar), `ktest_death_names` (the list read),
   `judge_ktest_death` (marker, then the crash report grammar, no prompt in
   between), `run_death_tests` (the per-test reboot loop), `ends_machine`
   learns `ktest death` ends the machine.
6. `scripts/test_kshell_run.py`: the death verdict grammar cases.
7. Documents: the design doc's death paragraph, the README's user-facing
   section, and the change record in `docs/changes/261108/kernel-test-framework.md`.

## Files

- `kernel/lib/ktest/include/ktest/ktest.hpp` — the macro.
- `kernel/kernel.ld` — the section.
- `kernel/lib/ktest/src/runner.cpp` — the listing and the selection.
- `kernel/tests/ktest_death_test.cpp`, `kernel/tests/BUILD.gn` — the smoke.
- `scripts/kshell_run.py`, `scripts/test_kshell_run.py` — the host loop and
  its unit suite.
- `kernel/lib/ktest/README.md`, `docs/design/261105/kernel-test-framework.md`,
  `docs/changes/261108/kernel-test-framework.md` — the documents.

## Test plan

- Host: `python3 -I scripts/test_kshell_run.py` — the death verdict grammar
  cases green, the old 21 intact.
- ELF: `__ktest_death_start != __ktest_death_end` in `vmours.with-tests`
  (dbg, exactly one record); neither it nor `__ktest_death_end` present in
  the plain `vmours` (rel), the `.hsien.tests` rule.
- Boot: `python3 .agents/skills/verify/scripts/boot_check.py` green.
- On the dbg image: `ktest list` ends with the DEATH line; `ktest all` stays
  `ran=77 passed=75 failed=0 skipped=2`; `ktest death` reboots once, panics
  with the expected message, host verdict PASS; a filter that matches nothing
  fails without a reboot.
- clang-tidy on the touched C++ files; `format_cpp.py`/`format_gn.py`/
  `lint_docs.py` clean.

One commit: `feat(ktest): death tests, one fresh boot per test`.
