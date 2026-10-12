# kernel-task changes

## A CPU's Dpc workers can be stopped and started again (2026-10-12)

**Files**
- `kernel/include/ours/dpc.hpp` — `DpcRunner::stop_workers()`; `Queue::stop()`, `m_stopped`, `m_stopping`
- `kernel/kernel/task/dpc.cpp` — the worker leaves its loop at the flag, read where it pops a Dpc; `Queue::start()` clears the flag; `Queue::stop()` and `stop_workers()`
- `kernel/commands/dpc_test.cpp`, `kernel/commands/BUILD.gn` — `dpc.test`
- `.agents/tasks/cpu-hotplug/DESIGN.md`, `.agents/tasks/cpu-hotplug/PLAN.md` — the hotplug task: Zircon's sequence, and Zircon's rule that no thread pinned to a CPU going offline may wake there

**Symptom**
None, a new capability: step 1 of `.agents/tasks/cpu-hotplug/PLAN.md`. A CPU's Dpc workers, pinned to it, ran forever; under the rule chosen for hotplug no pinned thread may be left on a CPU going offline, so they must be able to stop.

**Cause**
None, a new capability.

**Change**
The objects:
- A **Dpc queue**: one per level per CPU, its list under a spinlock, an Event the worker sleeps on, and one worker thread pinned to the CPU.
- **`m_stopping`**: a flag under the queue's lock, set by `stop()`.
- **`m_stopped`**: an Event the worker signals as it leaves its loop.

The events, for one queue:
1. `stop()` sets `m_stopping` and signals the worker's Event, in one critical section.
2. The worker, at its next look at the list, reads the flag in the critical section where it would pop a Dpc, and leaves without popping.
3. It signals `m_stopped` as its last touch of the queue and returns, so its thread exits.
4. `stop()` returns once `m_stopped` is signalled; the queue has no worker.
5. Dpcs enqueued from then on, and any the worker had not popped, stay in the list.
6. `start()` clears the flag and spawns a worker, which finds the Event still signalled and drains the list.

```cpp
// do_work(), under m_lock, before popping
if (m_stopping) {
    break;  // what is left stays for the next worker
}
...
// after the loop
m_stopped.signal();
return 0;
```

Why a flag and an Event, and not `join()`: every thread's exit queues the Dpc that reaps it on its own CPU's Normal queue (`Thread::Current::exit()`). The Normal worker's own reap Dpc would go to the queue it has just stopped serving, so joining it would wait forever. The workers stay detached, as they were, and each is freed by its reap Dpc once the CPU has a Normal worker again; when the CPU goes offline later in the task, that Dpc moves with the rest of the queue.

A Dpc is lost or run twice by neither: the flag is read in the same critical section as the pop, under the lock `enqueue()` and `stop()` take. A Dpc popped before the flag was seen runs once on the old worker; one not popped stays in the list for the next.

**Reference**
Zircon `DpcRunner::Shutdown()` (`zircon/kernel/kernel/dpc.cc`), which stops and joins a CPU's DPC threads before `mp_unplug_cpu_mask_single_locked()` takes the CPU down, and `DpcRunner::InitForCurrentCpu()`, which starts them again when it returns. Zircon joins because its thread exit does not depend on a DPC of the same CPU; here it does, hence the Event.

**Verification**
- Built in `.worktrees/wip-proc-self` at 1c6e4b7c plus only this change; clang-tidy clean on the changed files.
- `boot_check.py`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped), host tests 144 passed.
- `scripts/kshell_run.py dpc.test threadjoin.test` on dbg: "dpc.test: ok -- 9 checks" on CPU 3, then threadjoin's 11 checks pass.
- Mutations: with the flag not cleared in `start()`, the restarted worker leaves at once and three checks fail (the queued Dpc never runs, the join times out); with the flag ignored, `stop_workers` answers TimeOut.

**Deliberately not done**
- Handing a stopped CPU's queued Dpcs to another CPU: the offline step does it, once the CPU has halted (Zircon `DpcRunner::TransitionOffCpu()`).
- Stopping the levels in parallel: one after another is enough for a call that runs once per offline.
- `start_workers()` still asserts on a failed spawn, as at boot.

**How it was found**
None.

**The user's prediction**
None. The concurrency argument for this step (a Dpc enqueued while its worker stops) was deferred by the user ("先把作业留着，我后面一起做", 2026-10-12) and is listed in PLAN.md.

**Three questions you should be able to answer**
1. Why can `stop_workers()` not join the Normal worker, and what frees that worker's thread instead?
2. What guarantees a Dpc enqueued while the worker is stopping is neither lost nor run twice?
3. Why does the restarted worker drain the backlog without anyone signalling its Event again?
