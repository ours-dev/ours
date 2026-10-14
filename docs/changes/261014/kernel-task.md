# kernel-task changes

## A CPU going offline wakes the CPUs it moves its threads to (2026-10-14)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` — `Scheduler::Current::deactivate()` collects the CPUs it moved threads to and calls `reschedule_mask()` on them
- `kernel/commands/hotplug_test.cpp` — `check_moved_to_idle()`: a thread moved to an idle CPU runs there at once

**Symptom**
A thread running on a CPU as it went offline, moved to a CPU that was idle, sat off any CPU for 637 ms and 777 ms in two runs of the new check: until the idle CPU's next interrupt, its once-a-second lockup heartbeat.

**Cause**
`deactivate()` moved each thread with `activate_thread()` and `unpark_preempt()` on the target, under the target's lock, and told the target nothing. `unpark_preempt()` only records a request on the target's running thread, and for a thread of the same discipline not even that. A wakeup goes on to `reschedule_mask()`, which sends the target an IPI (`Scheduler::unpark_thread()`); the move did not. A busy target found the thread at its running thread's slice end. An idle one halted until some interrupt came. While every CPU ticked a hundred times a second that was within 10 ms, and nobody saw it. Idle-power step 4 removed the tick, and the wait became up to a second. Step 4 had named this kind of dependence, on "some interrupt will come soon", as the thing to look for, and this one was missed.

**Change**
Before:

```cpp
find_active_scheduler_for(thread, [](Thread *const moved, Scheduler *const target) {
    ...
    target->activate_thread(moved, EnqueueOptions::None);
    target->unpark_preempt(moved, UnparkOptions::None);
});
```

After, each target is collected and all of them told once the loop is done, with no run queue lock held, as `process_migrating_threads()` does:

```cpp
moved_to.set(find_active_scheduler_for(thread, [](...) { ... }));
...
reschedule_mask(moved_to);
```

`reschedule_mask()` runs here in the idle thread, interrupts off, which only sends IPIs.

**Reference**
Zircon `Scheduler::MigrateUnpinnedThreads()` (`zircon/kernel/kernel/scheduler.cc`) ends with `RescheduleMask()` over the CPUs it moved threads to.

**Verification**
- Built in `.worktrees/wip-proc-self` at c65a75cc plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- `hotplug.test` before the fix: the new check fails twice, 777 ms and 637 ms. After it, three runs on dbg: ok, 40 checks, the thread off a CPU for 124, 186 and 220 µs; on the periodic build 113 µs; without `cpu_hotplug`, ok.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed.

**Deliberately not done**
None.

**How it was found**
Reading the wakeup paths for idle-power step 5 (MWAIT): an idle CPU there is woken by a write to a word it watches, or by an IPI, and `deactivate()` sent neither. The new check reproduced it before the fix.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did this not show while the tick ran, and what bounded the wait then?
2. Why is `reschedule_mask()` called after the loop rather than inside the lambda?
3. What does `unpark_preempt()` do for a thread of the same discipline as the target's running thread, and why is that not enough?
