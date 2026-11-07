# kernel-test-framework outstanding repairs

## 1. Intentional allocation-failure logs make a passing KTEST run fail the console runner

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

## 2. `kshell_run.py` reports a migrated test's old command name as a hang

**Severity**
Low: the runner's verdict (FAIL) is right, but its message is the same as for a real hang, and it cost a session a false "hangs when run alone" report on 2026-11-05.

**Symptom**
`python3 scripts/kshell_run.py --out out/<dir> waitqueue.test --command-timeout 60` fails every time with `no "waitqueue.test: ok" or FAILED line within 60s`, after the full 60 seconds. `semaphore.test`, `dpc.test` and `tickbcast.test` fail the same way, and so does any batch that names one of them (a fifteen-command batch stops at its first, `semaphore.test`, and the commands after it never run). There is no hang: the serial log shows `(kshell) run 'waitqueue.test' with 0 argument(s)` and then the `kshell> ` prompt, with no `waitqueue.test` line in between or after, for the rest of the 30 seconds. `ktest WaitQueue.*` passes in about 12 ms, six runs in a row on a four-CPU dbg image, and `ktest all` passes with `ran=24 passed=23 failed=0 skipped=1`.

**Cause**
Batch 1 of the console-test migration (`f44d9634`, 2026-11-05) removed the `waitqueue.test` command (and `dpc.test`, `tickbcast.test`, and with the pilot `semaphore.test`) and replaced them with `WaitQueue.*`, `Dpc.*`, `TickBroadcast.*` and `Semaphore.*` ktests. `gktl::run_command()` says nothing about a name it does not know (`kernel/lib/kshell/kshell/shell.cpp` says so itself), so the shell just prints its prompt. `kshell_run.py` waits for a `<name>: ok|FAILED` verdict only, and does not look at the prompt coming back first, so a name that no longer exists is indistinguishable from a command that never finishes. Anyone, or any script or document, still using an old name gets this message.

**Change**
Make the failure say what happened. Smallest: when a `.test` command times out and `"kshell> "` was printed again after the command was typed, say `the shell returned to its prompt without a verdict: no such command? a migrated test runs as `ktest <Suite>.*`` instead of the generic message. Larger: fail at once, after a short grace for buffered log lines, instead of waiting out `--command-timeout`; this needs a measurement of how late a verdict can land after the prompt on the dbg image (its docstring says the prompt and other CPUs' log lines can interleave) before it is safe, so it is not part of the smallest fix. Ruled out: keeping the old names as aliases of `ktest`, which would hide that the migration happened and keep the `Tally` convention the migration is deleting.

Also stale after the migration, to fix in the same pass: the comment at `kernel/kernel/sync/semaphore.cpp:38` ("semaphore.test (kernel/commands)"), the one at `kernel/tests/semaphore_test.cpp:41` ("waitqueue.test's"), and any task document or script that still runs the old names; `docs/changes/261105/kernel-test-migration.md` already says the old names are gone.

**Reference**
None, this project's own test runner.

**Acceptance**
`python3 scripts/kshell_run.py --out out/bringup.x86 waitqueue.test` fails with a message that names the missing command and `ktest`, not only a timeout; `ktest WaitQueue.*` still passes; a real hang of a `.test` command (for example `lockup spin all` style silence) is still reported as before.
