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

## A refused preemption deadline is passed on by a timer IPI to the CPU itself (2026-10-26)

**Files**
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp` — `TimerQueue::set_preemption_deadline()` and its comment
- `kernel/commands/tick_replace_test.cpp` — `tickreplace.test`'s sixth case, and `LateRefusingDevice`

**Symptom**
Review finding M2 of `.agents/tasks/idle-power/REVIEW.md`, now reproduced. A CPU whose tick device starts refusing, when the first deadline it refuses is a preemption deadline, stays on that device with no tick. `tickreplace.test`'s new case shows it: without the fix the CPU was still on the refusing device when the case gave up after 5 s (`CPU[3] tick moved from late-refusing to LVT` comes only from the test's cleanup, 5.0 s after the refusal), and three checks failed. Left alone, nothing ends the running thread's slice, no timer on the CPU fires, and the lockup detector reports the CPU after ten seconds.

**Cause**
`set_preemption_deadline()` is called by every pick of a thread, under the run queue lock with interrupts off. A refusal there cannot be passed on in place: `notify_refusal()` answers it by asking the device again and swapping devices, which takes a broadcast group's lock and logs, and may wake a thread, which needs the run queue lock this CPU already holds. So it raised `m_refused` and left the refusal for "the next visit to this queue", the next activation, cancellation or expiry of a timer on that CPU. The periodic tick used to guarantee such a visit within 10 ms. Tickless one-shot mode (idle-power step 4) took the tick away, and a thread that computes and arms no timer makes no visit, so with the device refusing there is nothing left to make one.

**Change**
After dropping the queue lock, `set_preemption_deadline()` looks at `m_refused` and, if it is up, sends a timer IPI to its own CPU (`IpiService::send_event(Cpu::id(), IpiEvent::Timer)`). The IPI arrives as soon as interrupts are back on, after the switch. Its handler (`TickBroadcast::handle_ipi()`) runs the queue, whose last step reprograms the device and calls `notify_refusal()` with no lock held. That is the visit the tick used to provide, made on demand. The flag is read with interrupts off, as the caller has them, so nothing on this CPU can take it between the read and the send. The common case pays one relaxed load of a line the lock hold has just touched.

The review proposed a different place: call `notify_refusal()` where the scheduler has dropped the run queue lock, after `reschedule_common()`'s early returns and in `complete_context_switch()`. Not taken, for two reasons. First, there is no single such place. A switched-in thread resumes inside `reschedule_common()`, a new thread in `trampoline_handoff()`, and every early return is another exit. Each would need the call, and an exit added later without it would bring the bug back silently. Second, `complete_context_switch()` still runs with the incoming thread's PI lock held (`trampoline_handoff()` and the caller's guard release it afterwards), and a device swap that wakes a thread would then take locks under that one. The self IPI needs one call site, and the hardware supplies "the first instant interrupts are on" whatever path the code takes to get there.

One consequence: the device is asked once more before it is retired. The pick's refusal is the first ask, the IPI's pass reprograms and is refused again, then `on_device_refused()` asks twice more, four asks in all where a refusal from a timer makes three. Harmless, and the count `tickreplace.test`'s first case checks is unchanged, because that case's refusal comes from enrollment and not from a pick.

**Reference**
Linux's `irq_work` is the same pattern: work raised in a context that cannot do it (NMI, or under a lock) is queued and a self IPI is sent (`irq_work_queue()` → `arch_irq_work_raise()`, `arch/x86/kernel/irq_work.c`), so it runs at the first point interrupts are enabled. Zircon has no counterpart, because its `platform_set_oneshot_timer()` cannot fail and `TimerQueue::UpdatePlatformTimerMono()` (`zircon/kernel/kernel/timer.cc`) ignores the question. Refusal and device replacement are this project's own, after Linux's `clockevents_exchange_device()`.

**Verification**
- Built in `.worktrees/wip-proc-self` at 21a17c02 plus only this change. `boot_check.py`: dbg and rel PASS, host tests 147 passed.
- `tickreplace.test` ok in three separate boots, 26 checks. `timer.test`, `tickfallback.test`, `tickbcast.test`, `timershare.test`, `hotplug.test` and `idle.test` ok. clang-tidy reports nothing on the changed files.
- Mutation: with the send removed, the sixth case fails its three checks (back on its own device, device marked failed, timers fire), and nothing else does.
- `preempt.test` fails when it runs after `tickreplace.test` in the same boot, and it fails the same way at 21a17c02 without this change, so this change did not cause it. Recorded as item 1 of `docs/repairs/261026/kernel-time.md`.

**Deliberately not done**
A CPU whose tick device has failed with no replacement left ("the CPU has no tick") refuses every deadline. It now gets one self IPI per pick, each running `on_device_refused()` and its warnings. It had no tick before this change too, and its log already filled on each visit to its queue. Making that state quiet is a separate question.

**How it was found**
The architecture review of 2026-10-26, finding M2.

**The user's prediction**
None, homework paused.

**Three questions you should be able to answer**
1. Why can `set_preemption_deadline()` not call `notify_refusal()` itself, and why did "the next visit to the queue" stop being enough once the tick was gone?
2. Why does a self IPI reach the refusal on every path out of a reschedule, where a call placed after the scheduler drops its lock would have to be repeated at each exit?
3. Why is it safe to read `m_refused` after the queue lock is dropped, and still be sure the IPI is sent for the refusal just recorded?
