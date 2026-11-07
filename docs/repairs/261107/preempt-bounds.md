# preempt-bounds outstanding repairs

## 1. Preempt test's lateness bounds predate the periodic-balance scheduler

**Severity**
Low: the suite is skipped (`KTEST_SKIP`) in kernel/tests/preempt_test.cpp, so nothing runs red; but one of the migration's eight checks cannot run until this is fixed.

**Symptom**
`ktest Preempt.*` (or the old `preempt.test`) fails on this host, deterministically: the longest spinner turn ran 68371 us against a slice + late bound of 68000 (slice 64000, late 4000), and the probe timer fires up to 2608 us late against a 2000 bound. The migration's baseline control ran the OLD console command on baseline and it failed the same check the same way: the bounds were measured on 2026-11-05 against a quieter scheduler, and the periodic balance (form T) and the steal work landed after.

**Cause**
The bounds are constants in the test, measured when the test was written; the scheduler they measure changed under them. Not a defect in the scheduler either, until the re-measurement says the turn genuinely overruns -- which would be one.

**Change**
None yet. The fix is a re-measurement on the current scheduler and new constants with the evidence beside them (docs/perf/<date>/preempt-bounds/), not a silent widening: the two bounds are `MAX_SLICES_IN_A_ROW`'s turn-length window (slice + late, times the slices in a row) and `PROBE_LATE` (the probe timer's fire lateness). If the new scheduler genuinely lets a turn run past slice + late rather than the measurement being noisy, that is a scheduler finding, not a test constant.

**Reference**
Linux `sched_tick()`'s slice checks and the rt-period accounting; the closest local: timer.test's lateness bound, which the same scheduler shift may also move.

**Acceptance**
`ktest Preempt.*` passes on the dbg image three runs in a row with every check enabled (no SKIP), and the old console command is gone (the migration's acceptance).
