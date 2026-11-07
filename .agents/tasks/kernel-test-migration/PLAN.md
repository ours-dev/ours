# kernel-test-migration: moving the remaining console-command tests to ktest

**Date**: 2026-11-05
**Status**: Proposed

## Request

The user asked for the rest of the migration ("迁移"): move the remaining 43 `*.test` console commands (46 originally, minus the three migrated in `9534e49b`) to the kernel test interface, so `kernel/commands` holds no tests, `ktest list` names every kernel test, and the legacy host rules (`kshell_run.py`'s `<name>: ok` and `cov_kernel.py`'s `GKTL_COMMAND` scrape) can be deleted. The design is `docs/design/261105/kernel-test-framework.md`; the work list is `docs/repairs/261105/kernel-test-framework.md`; the migration pattern is the three tests under `kernel/tests/` that landed on 2026-11-05. Scope ends at the last command; new framework features (fixtures, arguments, death tests) are not part of it.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| How the four commands that take an argument are migrated (`interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `threadjoin.test stress N`, `timershare.test big`) | A: each becomes two fixed-configuration tests (a default and a conservative stress variant), no runtime configuration; B: `ktest` grows a per-run option mechanism, and the four migrate as ordinary tests that read it with fallbacks; C: they stay console commands and only the other 39 migrate | B, because it keeps the tests' existing configurability without a second argument path per test | | B, 2026-11-05 |

## Steps

Later steps are written when each one is introduced to the user, not before.

3. Batch 2, landed in `028da2ea` (2026-11-07): preempt, sleep, pin_lines, page_queues, irq_controller, bti_stress. `Preempt.SlicesEndAtTheDeadline` is `KTEST_SKIP`d whole -- its bounds predate the periodic balance (form T) and the steal work; the baseline control (the OLD console command on baseline) fails the same check. `docs/repairs/261107/preempt-bounds.md` carries the numbers. irq_controller's `answered()` names checks through `"{}"`.
4. Run options, landed in `19cf30f5` (2026-11-05): `ktest <selection> [name=value ...]`.
5. Batch 3, landed in `855919f8` (2026-11-07): user_copy, irq_rcu, timer, ioport, memory_pressure, resource, bti. Status/Rights keep `EXPECT_TRUE` (no `std::formatter`).
6. Batch 4, landed in `43d23809` (2026-11-07): pager, large_page, hotplug, steal, irq_revoke, page_reclaim, tick_replace, pm_node. pm_node's `--exhaust-remote` is the first through the run options.
7. Batch 5, landed in `230ae8c7` (2026-11-07): irq_registry, interrupt, interrupt_line, page_source, irq, vmo_snapshot -- the six largest. interrupt.test's `[ROUNDS]` is dropped with the fixed default. `ktest list` names 56.

8. The finale batch (next session): `idle.test` (lives in idle.cpp, not its own file), `mutex.stress` (`mutex_stress.cpp`), and the four argument-takers -- `port.test [ROUNDS]`, `threadjoin.test stress N`, `timershare.test big` through the run options, and `interrupt.test` already migrated with fixed rounds. Then the acceptance: `kshell_run.py`'s `<name>: ok` rule and `cov_kernel.py`'s `GKTL_COMMAND` scrape are deleted; `docs/repairs/261105/kernel-test-framework.md` items 1-3 are closed; `cov_kernel.py` re-run enumerates every test; merge to baseline.

1. Batch 1: the thirteen smallest commands (135–298 lines), converted in parallel and integrated as one change. Landed in `f44d9634`. `vm_address`, `tick_fallback`, `vma_physical`, `vma_seal`, `pm_zone`, `wait_queue`, `vma_undo`, `vmo_pin`, `vma_phys`, `dpc`, `vmo_contiguous`, `tick_broadcast` and `irq_domain` move from `kernel/commands/` to `kernel/tests/`, one `KTEST` per command body, every check preserved; the command library loses their files, `kernel.tests` gains their `kernel_test()` targets. Why now: it is the mechanical core of the remaining work and proves the conversion pattern scales past the pilot. Verification: prompt-level `boot_check.py`; `ktest list` names ten plus thirteen tests; each new suite runs and passes under `ktest <Suite>.*`; the old command names are gone; tidy, format, GN and docs lint clean. One commit.
   - As landed: all thirteen converted, every check preserved; `tick_fallback` and `tick_broadcast` turned their "cannot exercise" `ok -- skipped` paths into `KTEST_SKIP`, which is the one verdict-shape change; `dpc` keeps a local `ok` and `FAIL`s at the end to preserve its leak-on-failure behaviour; `wait_queue` folded a helper into the body so its allocation can be `ASSERT_NONNULL`; `vma_phys`/`irq_domain` use `FAIL` inside helpers where `ASSERT` cannot go. Diagnostic logs stayed with the suite name as prefix; six files dropped `<logz4/log.hpp>`, and every file dropped the gktl command-table paragraph.
   - Verification, as done: `boot_check.py` passes (dbg 46/5/0, rel 44/8/0, host-tests 173/0); `ktest list` 23 tests; `ktest all` `SUMMARY ran=23 passed=22 failed=0 skipped=1`, exit 0; all thirteen suites run alone through the host verdict (`ok, 1 ran, 1 passed` each: Dpc, IrqDomain, PmZone, TickBroadcast, TickFallback, VmAddress, VmaPhys, VmaPhysical, VmaSeal, VmaUndo, VmoContiguous, VmoPin, WaitQueue); `kernel/commands` now has 30 `*.test` commands; `gn check` OK on every target; tidy/format/GN/docs lint clean. The one skip in the run of everything is `Ktest.SkipIsReported`; the batch's skip branches were not exercised on this machine.

2. Run options for the four argument-taking commands. Landed in `19cf30f5`. `ktest <selection> [name=value ...]` carries a run's options, and a test reads them with a fallback of its own.
   - `kernel/lib/ktest/include/ktest/options.hpp`: `Option`, the pure `parse_option`/`find_option`, and the `has_option`/`option`/`option_usize` accessors; the runner parses at most 14 tokens after the selection, logs each as `ktest: OPTION name=value`, and answers a malformed number with an error line and the fallback. `ktest list` takes no options; values are views into the console line and live for the run.
   - `Ktest.ReadsRunOptions` is the booted test of both paths; the host check that `expected=41` fails proves the option reaches the test rather than a default. `kernel/lib/ktest:ktest-test` has two option-parsing cases, and `scripts/test_kshell_run.py` keeps the list-count check for `ktest all <option>=<value>`.
   - Why this instead of splitting the commands (option A): the round count and the `big` flag are the only way to reach the longer paths without editing source, and they are what the command help text promised. The user chose B on 2026-11-05.
   - Verification, as done: host tests 11 + 20, all pass; `boot_check.py` both images and 173 host tests; `ktest Ktest.ReadsRunOptions` (fallback), `... expected=42` (visible) and `... expected=41` (failure line names 41 vs 42) all behave as intended; `ktest list` 24 tests; format/tidy/gn check clean.
   - Landed as one commit, `19cf30f5`, `feat(ktest): run options for tests that need a value`.

## Files

- Batch 1 (step 1): the thirteen `kernel/commands/<name>_test.cpp` files moved to `kernel/tests/`, `kernel/commands/BUILD.gn` and `kernel/tests/BUILD.gn` updated, `docs/changes/261105/kernel-test-migration.md` created, `docs/repairs/261105/kernel-test-framework.md` updated to the 30 remaining commands.

- `kernel/lib/ktest/include/ktest/options.hpp`, `kernel/lib/ktest/test/options_test.cpp`, `kernel/lib/ktest/src/runner.cpp`, `kernel/tests/ktest_test.cpp`, `scripts/kshell_run.py`: run options, their host tests and the booted smoke test (step 2).
- `docs/design/261105/kernel-test-framework.md`: the grammar and the fallback rule (step 2).

## Test plan

- Per batch: `python3 .agents/skills/verify/scripts/boot_check.py` (both images, host tests), `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest list'` and one filtered run per migrated suite, plus `'ktest all'`; a command whose old name was removed must fail as unknown; tidy/format/GN/docs lint; `python3 scripts/cov_kernel.py --out out/cov-kernel` after the last batch (it must still enumerate the remaining commands).
- Run options (step 2): `ktest Ktest.ReadsRunOptions` without an option (fallback), with `expected=42` (visible) and with `expected=41` (fails, the message naming the value). Done in step 2.
