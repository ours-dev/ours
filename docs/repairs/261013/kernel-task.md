# kernel-task outstanding repairs

## 1. Taking a busy CPU offline waits for its Dpc workers to get the CPU behind every runnable thread there

**Severity**
Low: a latency, not a wrong result; the offline completes.

**Symptom**
In `hotplug.test` on dbg, with two spinning threads per CPU, `take_cpu_offline()` took about 330 ms the first time and about 140 ms the second, most of it in `DpcRunner::stop_workers()`.

**Cause**
Each Dpc worker has to run to see its stop flag, and the round-robin discipline has no priority: a worker woken on a CPU with runnable threads waits its turn behind them, a time slice each, and the three levels are stopped one after another.

**Change**
A performance item, so it goes through the user (CLAUDE.md, "Working with the user"). Measure first: time from `stop_workers()` to each worker's exit against the number of runnable threads on the CPU. Candidates:
- stop the three levels together, asking all before waiting for any;
- have the idle thread's offline transition stop the workers, since by then it has the CPU;
- run the workers at a priority above ordinary threads, once a discipline has priorities.

**Reference**
Zircon `DpcRunner::Shutdown()`, whose per-CPU DPC thread runs at `DPC_THREAD_PRIORITY`, defined as `HIGH_PRIORITY` (`zircon/kernel/kernel/dpc.cc:25`), above ordinary threads.

**Acceptance**
The measurement above, and the user's choice among the candidates.
