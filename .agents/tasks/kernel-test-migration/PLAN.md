# kernel-test-migration: moving the remaining console-command tests to ktest

**Date**: 2026-11-05
**Status**: Proposed

## Request

The user asked for the rest of the migration ("迁移"): move the remaining 43 `*.test` console commands (46 originally, minus the three migrated in `9534e49b`) to the kernel test interface, so `kernel/commands` holds no tests, `ktest list` names every kernel test, and the legacy host rules (`kshell_run.py`'s `<name>: ok` and `cov_kernel.py`'s `GKTL_COMMAND` scrape) can be deleted. The design is `docs/design/261105/kernel-test-framework.md`; the work list is `docs/repairs/261105/kernel-test-framework.md`; the migration pattern is the three tests under `kernel/tests/` that landed on 2026-11-05. Scope ends at the last command; new framework features (fixtures, arguments, death tests) are not part of it.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| How the four commands that take an argument are migrated (`interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `threadjoin.test stress`, `timershare.test big`) | A: each becomes two fixed-configuration tests (a short default and a long/stress one with the value the code already defaults to or the largest the boot can run), so no runtime configuration is needed; B: `ktest` grows a per-run configuration mechanism; C: they stay console commands and only the other 39 migrate | A, with the long variant's value chosen conservatively for QEMU | | |

## Steps

Later steps are written when each one is introduced to the user, not before.

1. Batch 1: the thirteen smallest commands (135–298 lines), converted in parallel and integrated as one change. Landed in `f44d9634`. `vm_address`, `tick_fallback`, `vma_physical`, `vma_seal`, `pm_zone`, `wait_queue`, `vma_undo`, `vmo_pin`, `vma_phys`, `dpc`, `vmo_contiguous`, `tick_broadcast` and `irq_domain` move from `kernel/commands/` to `kernel/tests/`, one `KTEST` per command body, every check preserved; the command library loses their files, `kernel.tests` gains their `kernel_test()` targets. Why now: it is the mechanical core of the remaining work and proves the conversion pattern scales past the pilot. Verification: prompt-level `boot_check.py`; `ktest list` names ten plus thirteen tests; each new suite runs and passes under `ktest <Suite>.*`; the old command names are gone; tidy, format, GN and docs lint clean. One commit.
   - As landed: all thirteen converted, every check preserved; `tick_fallback` and `tick_broadcast` turned their "cannot exercise" `ok -- skipped` paths into `KTEST_SKIP`, which is the one verdict-shape change; `dpc` keeps a local `ok` and `FAIL`s at the end to preserve its leak-on-failure behaviour; `wait_queue` folded a helper into the body so its allocation can be `ASSERT_NONNULL`; `vma_phys`/`irq_domain` use `FAIL` inside helpers where `ASSERT` cannot go. Diagnostic logs stayed with the suite name as prefix; six files dropped `<logz4/log.hpp>`, and every file dropped the gktl command-table paragraph.
   - Verification, as done: `boot_check.py` passes (dbg 46/5/0, rel 44/8/0, host-tests 173/0); `ktest list` 23 tests; `ktest all` `SUMMARY ran=23 passed=22 failed=0 skipped=1`, exit 0; all thirteen suites run alone through the host verdict (`ok, 1 ran, 1 passed` each: Dpc, IrqDomain, PmZone, TickBroadcast, TickFallback, VmAddress, VmaPhys, VmaPhysical, VmaSeal, VmaUndo, VmoContiguous, VmoPin, WaitQueue); `kernel/commands` now has 30 `*.test` commands; `gn check` OK on every target; tidy/format/GN/docs lint clean. The one skip in the run of everything is `Ktest.SkipIsReported`; the batch's skip branches were not exercised on this machine.

## Files

- Batch 1 (step 1): the thirteen `kernel/commands/<name>_test.cpp` files moved to `kernel/tests/`, `kernel/commands/BUILD.gn` and `kernel/tests/BUILD.gn` updated, `docs/changes/261105/kernel-test-migration.md` created, `docs/repairs/261105/kernel-test-framework.md` updated to the 30 remaining commands.

## Test plan

- Per batch: `python3 .agents/skills/verify/scripts/boot_check.py` (both images, host tests), `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest list'` and one filtered run per migrated suite, plus `'ktest all'`; a command whose old name was removed must fail as unknown; tidy/format/GN/docs lint; `python3 scripts/cov_kernel.py --out out/cov-kernel` after the last batch (it must still enumerate the remaining commands).
