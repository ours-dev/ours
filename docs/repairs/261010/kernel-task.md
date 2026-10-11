# kernel-task outstanding repairs

## 1. A join waits for the exit Dpc to be scheduled, so `threadjoin.test stress 2000` takes 140 s where an early signal takes 5 s

**Severity**
Low: nothing is wrong and nothing hangs, but a join's latency is bounded by when the dying thread's CPU gets round to its Dpc worker, which caps how fast threads can be joined and freed.

**Symptom**
On the dbg image (4 CPUs under QEMU), `threadjoin.test stress 2000` reports `stress: 2000 round(s) x 4 thread(s) in 140509 ms, 0 bad`. That is about 70 ms per round, and it exceeds `kshell_run.py`'s default 120 s command timeout, so it has to run with `--command-timeout 300`.

The same test took:
- 4813 ms when `exit()` signalled `exited()` itself, before its final switch, with the thread kept alive by a second reference (`9f78ed1e`, reverted on 2026-10-10);
- 6952 to 8440 ms with the JoinHandle on top of that (`e9e44a50`, also reverted).

**Cause**
Not measured yet; this is the hypothesis to test first. The joinable path of `Thread::Current::exit()` signals `exited()` from a Dpc. That Dpc runs only when the DEFAULT-priority Dpc worker pinned to the dying thread's CPU is next scheduled, after the final switch. Until then the joiner stays parked. The debug sample threads `Sample1` and `Sample2` (`kernel/top/main.cpp`) keep CPUs 2 and 3 busy printing, so the worker may wait a whole time slice behind them each round.

The reverted early signal did not wait for the worker: the thread woke its joiner itself, and the Dpc only freed the descriptor later.

**Change**
Measure before choosing, and take the options to the user (see "Working with the user" in CLAUDE.md).

The measurements to take first:
- For each round, the time from the target's `set_state_dead()` to the joiner's wake-up.
- The same split into the final switch, the Dpc worker's start and the signal.
- The same with the sample threads disabled.

Candidates, each with its cost:
- **Signal before the final switch, kept alive by a second reference.** This is `9f78ed1e`'s mechanism. It wakes the joiner at once, at the price of a reference count beside the Joined and Detached claims.
- **Run the exit Dpc at a higher Dpc level.** This keeps the current design, but competes with other high-level Dpcs and still waits for a switch.
- **Leave it.** Only a test that joins in a tight loop notices.

**Reference**
- **Zircon.** `Thread::Current::Exit()` wakes joiners while holding the thread's lock through the final reschedule (`kernel/kernel/thread.cc`), which needs its ChainLock; ours has none.
- **Linux.** `exit_notify()` notifies the parent before the final `__schedule()`, and the task stays alive through the scheduler's reference until `finish_task_switch()`.

**Acceptance**
- The measurement above, recorded with the chosen option.
- `threadjoin.test stress 2000` well inside the default 120 s timeout.
