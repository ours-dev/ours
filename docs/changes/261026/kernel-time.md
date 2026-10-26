# kernel-time changes

## One name for setting and clearing a CPU's preemption deadline (2026-10-26)

**Files**
- `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp` — `TimerQueue::set_preemption_deadline()`, `m_preemption_deadline`, `num_preemption_deadline_sets()`, `num_preemption_deadlines_passed()`, and the counters `timer.queue.preemption_deadline_sets` and `timer.queue.preemption_deadlines_passed`
- `kernel/include/ours/task/sched/sched_clock.hpp`, `kernel/kernel/task/sched/sched_clock.cpp`, `kernel/include/ours/task/sched/run_queue.hpp` — `set_preemption_deadline()` and `clear_preemption_deadline()`
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp`, `kernel/include/ours/task/sched/scheduler_inline.hpp` — `Scheduler::Current::on_preemption_deadline()`, and the calls
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/commands/preempt_test.cpp` — the names in comments and calls
- `benches/kernel/time/preemption_deadline_bench.cpp`, `BUILD.gn`, `benches/kernel/BUILD.gn` — `preemptreset.bench` is now `preemptdeadline.bench`, its cases `set same` and `set new`
- `.agents/skills/ours-kernel-design/references/task.md`, `docs/perf/261025/pick-cost.md` — the new names

**Symptom**
None. Review finding "Naming" of `.agents/tasks/idle-power/REVIEW.md`: one act had three names across three layers, `RunQueue::arm_preempt_timer()` and `disarm_preempt_timer()`, `SchedClock::reset_preempt()` and `cancel_preempt()`, `TimerQueue::reset_preempt()`. The hook that runs when it passes was `on_preempt_timer()`.

**Cause**
The names came from Fair's `arm_preempt_timer()`, written when the preemption deadline was a timer on the tick device, and from Zircon's `PreemptReset()`. Since idle-power step 3 it has been a slot in the timer queue, the earliest of whose deadlines arms the device. "Timer" named something that no longer exists, and "reset" and "cancel" named the same act as "arm" and "disarm".

**Change**
One vocabulary, the slot's own: `set_preemption_deadline()` and `clear_preemption_deadline()` in `RunQueue` and `SchedClock`. `TimerQueue::set_preemption_deadline()` takes the deadline itself, `INFINITE_TIME` clearing it. `Scheduler::Current::on_preemption_deadline()` runs when it passes. The counters say what they count. The bench was named after `reset_preempt()` and is renamed with it; the pick-cost report says so beside the command it was run with.

Fair's `set_next_thread()` still calls `arm_preempt_timer()`: `fair.*` is the user's, and out of the build.

**Reference**
None, naming.

**Verification**
- Built in `.worktrees/wip-proc-self` at d927b3fc plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- `boot_check.py`: dbg and rel PASS, host tests 147 passed.
- `preempt.test`, `hotplug.test` and `idle.test` ok; `preemptdeadline.bench`: 1.00 deadline sets and device writes per switch.

**Deliberately not done**
Fair's call, as above.

**How it was found**
The architecture review of 2026-10-26.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did "timer" stop being the right word for the preemption deadline?
2. Where is the deadline set, and where is it cleared?
3. Why was Fair's call left as it was?
