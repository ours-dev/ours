# sampler outstanding repairs

## 1. The sampler stress test rarely meets a writer mid-write while timers fire only on the tick

**Severity**
Low: a test weaker than it was written to be; the stop barrier's correctness rests on its argument, not on this test.

**Symptom**
`check_sampler_stress()` (`system/app/shell/shell_tests.cpp`) checks about 60 records per boot over 32 runs (`shell: sampler stress: 32 runs in 16 sessions, 59 records checked`). A stop lands on a write in time to be caught in roughly 0.2% of runs, so most boots never exercise the race the test is named for.

**Cause**
Software timers fire only on the 10 ms tick (HZ 100), so a session at the shortest period, 10 µs, really samples once a tick, and each run, a millisecond or two of work, takes at most one sample per CPU. Depends on `.agents/tasks/high-res-timers/DESIGN.md`.

**Change**
Once timers fire at their own deadlines, the runs at 10 µs fill their buffers and the stop meets live writers. Then, if the race still needs more pressure: rounds bound by time rather than by work, or an on-demand stress program run from the shell with thousands of runs, kept out of the boot (options B and C of 2026-10-09).

**Reference**
None, this project's own test.

**Acceptance**
After the high-res timer work, the stress line reports hundreds of records per boot; both images boot.
