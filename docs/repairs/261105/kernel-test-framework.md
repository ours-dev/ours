# kernel-test-framework outstanding repairs

## 1. The remaining kernel `*.test` console commands have not migrated to the kernel test interface

**Severity**
Low: the tests still run and pass, but each carries its own `Tally`/`verdict`/`ok` reporting boilerplate, none of them appears in `ktest list`, and `scripts/cov_kernel.py` still has to scrape their registrations to find them.

**Symptom**
`python3 scripts/kshell_run.py --out out/bringup.x86 'ktest list'` lists 23 tests (the ten of the pilot and batch 1, plus batch 1's thirteen), and the other 30 `*.test` commands still exist (`kernel/commands/*_test.cpp`, 350 to 2,266 lines). `kshell_run.py`'s `<name>: ok` rule and `cov_kernel.py`'s `GKTL_COMMAND` scrape keep a second path alive for them.

**Cause**
The interface's first slice ended with a three-test pilot (design, 2026-11-05), and the migration is incremental: batch 1 (the thirteen smallest, 135 to 298 lines) landed on 2026-11-05; the remaining 30 are the rest of that list, not a defect in any one file.

**Change**
Continue in size order. What is left is 30 commands: these 29 `*_test.cpp` files plus `idle.test`, which lives in `idle.cpp`, and then `mutex.stress` in `mutex_stress.cpp` for the same treatment. Next: `preempt_test.cpp` (350) and `sleep_test.cpp` (355), then `page_queues_test.cpp` (370), `pin_lines_test.cpp` (386), `irq_controller_test.cpp` (403), `bti_stress_test.cpp` (409), `thread_join_test.cpp` (418), `timer_share_test.cpp` (431), `user_copy_test.cpp` (448), `irq_rcu_test.cpp` (472), `timer_test.cpp` (474), `pm_node_test.cpp` (480), `port_test.cpp` (480), `ioport_test.cpp` (482), `memory_pressure_test.cpp` (508), `resource_test.cpp` (547), `bti_test.cpp` (610), `large_page_test.cpp` (625), `pager_test.cpp` (625), `hotplug_test.cpp` (656), `page_reclaim_test.cpp` (739), `irq_revoke_test.cpp` (746), `tick_replace_test.cpp` (826), `irq_registry_test.cpp` (947), `page_source_test.cpp` (1,091), `interrupt_test.cpp` (1,180), `interrupt_line_test.cpp` (1,218), `irq_test.cpp` (1,456), `vmo_snapshot_test.cpp` (2,266) -- 29 files; `kernel/commands` also holds `mutex_stress.cpp`, which is a `*.stress` command and needs the same decision.

Four commands take an argument and cannot migrate as they are: `interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `threadjoin.test stress`, `timershare.test big`. The interface has no per-run configuration; decide first whether each becomes two fixed-configuration tests (a default and a conservative stress variant, the recommendation), whether `ktest` grows an argument mechanism, or they stay commands. `timer_test.cpp` also reads boot options for its oneshot paths and skips when they are off; that is a skip, not an argument.

**Reference**
The migrated tests under `kernel/tests/` are the pattern, batch 1's thirteen included; the interface is `docs/design/261105/kernel-test-framework.md`.

**Acceptance**
`ktest list` names every kernel test; `kernel/commands` holds only commands that are not tests; `kshell_run.py`'s `<name>: ok` rule and `cov_kernel.py`'s `GKTL_COMMAND` scrape can be deleted; `python3 .agents/skills/verify/scripts/boot_check.py` passes and `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest all'` reports every test passing; `python3 scripts/cov_kernel.py` still enumerates every test at run time.

## 2. Intentional allocation-failure logs make a passing KTEST run fail the console runner

**Severity**
Low: the optional MM allocation-failure build reports a false harness failure, while the tests' own assertions and summary succeed.

**Symptom**
With `mm_test_alloc_failure=true`, `ktest all` reports `ran=24 passed=23 failed=0 skipped=1`. The seal suite intentionally causes three page-table allocation failures, whose production error logs say `Failed to map ... pages`. `scripts/kshell_run.py` then reports `3 FAILED or error line(s) beside the SUMMARY` and returns failure. The coverage runner accepts the zero-failure test summary. Observed during the MM integration on 2026-11-05.

**Cause**
`judge_ktest_run()` correctly rejects unaccounted error logs, but the MM injection tests currently have no explicit way to mark the expected diagnostics produced by the error path they are testing. A successful assertion does not by itself classify that independent log output.

**Change**
Provide a narrowly scoped expected-diagnostic mechanism for intentional allocation failures, or report those expected diagnostics through a test-owned sink. Do not remove generic error checking or whitelist every page-table failure: either would hide unexpected errors in other tests. This needs an interface decision before implementation.

**Reference**
None, this project's own diagnostic and kernel-test interfaces.

**Acceptance**
The default and fault-enabled `ktest all` commands both pass the console runner when their summaries have no failures. A deliberately unexpected error emitted by a passing test must still make the runner fail.
