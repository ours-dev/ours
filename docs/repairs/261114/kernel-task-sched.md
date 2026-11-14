# kernel-task-sched outstanding repairs

## 1. Steal.IdleCpuTakesQueuedThreadsByNodeAndAffinity fails now and then

**Severity**
Low: a test that fails without a change to what it tests, so a red run says nothing about the change under test.

**Symptom**
One of six runs on 2026-11-14 (`ktest all` on the default machine, `feat/pmm-redesign` at the watermark-admission change) failed with `steal_test.cpp:436: heavy.steals == u64{1}: expected 0, actual 1 -- the idle CPU takes the one waiting thread although the mean is above its ...`. Four runs of `ktest Steal.*` alone and the next full run passed on the same build.

**Cause**
Not established. The check counts steals in a window, and a steal that happens before or after the window it means to observe, or a CPU going idle at a different moment, changes the count. The change under test at the time touched only frame allocation, which the steal path does not call.

**Change**
Find what the test races with: loop `ktest Steal.*` under `kshell_run.py` until it fails, and read which CPUs were idle and when the steal counter moved. Then either make the check wait for the state it asserts or make the window explicit.

**Reference**
None, this project's own test.

**Acceptance**
`ktest Steal.*` passes fifty runs in a row on the default and the four-node machine.
