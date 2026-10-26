# idle-power: review of the architecture the CPU hotplug and idle-power work built

**Date**: 2026-10-26

## Scope and method
The design the two tasks (`.agents/tasks/cpu-hotplug`, `.agents/tasks/idle-power`) left across the kernel, at 775d175b:

- `kernel/kernel/time`: `TimerQueue` with the preemption-deadline slot and the hold-off another session added, `TickDevice`, `TickSched`, `TickBroadcast`;
- the scheduler's preemption path: `Scheduler::set_next_thread()`, `reschedule_common()`, `deactivate()`, `on_preempt_timer()`, `SchedClock::reset_preempt()`;
- `kernel/kernel/task/idle_thread.cpp`, `cpu_hotplug.cpp` and the hotplug hooks;
- the x86 idle: MWAIT and the waker (`kernel/arch/x86/kernel/task/idle_thread.cpp`, `mp.cpp`), `kernel/arch/x86/lib/idle_states`;
- the lockup heartbeat (`kernel/lib/lockup_detector/src/checker.cpp`).

Read against `modern-cpp-development`, `ours-kernel-design` (`references/task.md`: Scheduler, Time, CPU hotplug, Idle) and Zircon's `kernel/timer.cc`, `kernel/scheduler.cc` and `arch/x86/mp.cc`.

`clang-format --dry-run` is clean on the thirteen files written in this work. `clang-tidy` is clean on ten of them; `kernel/kernel/task/idle_thread.cpp` has two older naming warnings, at :150 and :155. The lock-order claims were checked by reading every holder of the timer queue's lock and of a broadcast group's lock. M1 and M3 follow from the code: the state they describe was traced, not reproduced. M2 and M4 are read-only judgement. The style counts are a grep of the files listed.

## What is done well
- **One writer for each CPU's timer device.** `TimerQueue::earliest_deadline_locked()` (`kernel/kernel/time/timer_queue.cpp:71`) folds the timers, the hold-off and the preemption deadline into one answer, and `reprogram_locked()` alone writes the device. The hold-off composes cleanly because it is applied to the timers and not to the slot (:92): a slow callback can delay timers, never the end of a slice.
- **A lock order that is one-way end to end.** The order is run queue, then timer queue, then broadcast group. No timer callback runs under the queue lock, and `on_preempt_timer()` is called after it is dropped. `TickBroadcast::on_device_event()` runs a queue only after releasing its group lock.
- **The hotplug hooks** (`kernel/kernel/task/cpu_hotplug.cpp:136`, `:147`). Levels are walked down on the way out and up on the way in, reverse within a level, so the symmetry is the walk's and not two hand-kept lists'. Subsystems depend on task, not the other way.
- **`deactivate()` wakes the CPUs it moves threads to** (`kernel/kernel/task/sched/scheduler.cpp:1057`, `:1066`), with a test that measures the wait (`hotplug.test`, `check_moved_to_idle()`).
- **The MWAIT wake protocol** (`kernel/arch/x86/kernel/task/idle_thread.cpp:119`, `:150`). Its order (store Waiting, MONITOR, check, MWAIT) closes each window a waker could fall into. Because the word says Waiting only inside `idle()`, a waker that finds it so knows exactly what the target will do next.
- **`CpuLocal::Current::IS_VALUE`** (`kernel/mm/include/ours/mm/cpu_local.hpp:676`): one named predicate replaces a size test that silently returned copies.
- **The idle loop's clock readings are reused.** The governor's two inputs come from the readings the halt accounting takes anyway (`kernel/kernel/task/idle_thread.cpp:73`, `:98`, `:224`), and the governor itself is a pure, host-tested function with its overflow guarded (`kernel/arch/x86/lib/idle_states/src/idle_states.cpp:76`).

## High severity
None.

## Medium severity
### M1. A CPU whose bring-up times out can never be used again, or comes up without its Dpc workers
`kernel/kernel/task/idle_thread.cpp:253`, `:262`; `kernel/kernel/task/cpu_hotplug.cpp:119`, `:123`. `transition(Running)` stores `{current: Offline, target: Running}`, starts the CPU, and waits one second. On a timeout it returns `TimedOut` and leaves the state as it is, and `bring_cpu_online()` returns without starting the CPU's Dpc workers. Two endings follow:
- **The CPU never arrives.** A retry finds `current == Offline`, calls `transition()` again, and gets `BadState`, because `current != target`. `take_cpu_offline()` answers `BadState`, because the CPU is not online. No call can leave that state.
- **The CPU arrives late.** `come_back()` sets `{Running, Running}`, and the CPU is online and active, but with its Dpc workers stopped since it went offline: a Dpc queued there never runs.

Fix: on a timeout, put the state back to `{Offline, Offline}` and hold the CPU again with an INIT, so that a retry starts clean. And start the Dpc workers from the CPU's own side, as an online hook or in `come_back()`, rather than from the caller after a successful wait, so that a late arrival still gets them.

### M2. A refused preemption deadline is acted on only at the queue's next visit, which on a busy tickless CPU may never come
`kernel/kernel/time/timer_queue.cpp:170`, documented at `kernel/include/ours/time/timer_queue.hpp:86`. `reset_preempt()` runs under the run queue lock, so it leaves a refusal for `notify_refusal()`, whose answer takes locks of its own. Until something calls it, the device may be unarmed. Nothing then ends the running slice, fires the CPU's timers, or beats its heartbeat. The next visit is the next activate, cancel or run of the queue on that CPU. A compute-bound thread makes none, and with no tick there is no interrupt left to make one, so the lockup detector reports the CPU after ten seconds. A refusal is rare, since the device has already retried at its minimum delta, but the tick used to bound this at 10 ms, and removing it removed the bound.

Fix: call `TimerQueue::get().notify_refusal()` where the scheduler has dropped the run queue lock: after `reschedule_common()`'s early returns and in `complete_context_switch()`. In the common case that costs one relaxed load.

### M3. Taking a CPU offline waits without bound, holding the hotplug lock that sampler control also takes
`kernel/kernel/task/cpu_hotplug.cpp:81` (`transition(Offline, Deadline::infinite())`), `:92` (a spin on `has_halted()`). If the target never reaches its idle loop, for instance a kernel thread spinning there with preemption off, the caller blocks for ever inside `HOTPLUG_LOCK`. So does every later hotplug, and every `sampler::Session` start, stop and release, which take `cpu_hotplug_lock()` first. The caller's `deadline` bounds only the Dpc workers' stop. Fix: bound both waits by `deadline`. On a timeout, put the target back to Running if it has not left, or report it lost if it is half gone, and restart its Dpc workers.

### M4. An online hook cannot fail, so a CPU can come online without a timer device
`kernel/kernel/time/tick_device.cpp:343`. The Tick level's online hook logs "came back without a tick" and carries on, because `CpuHotplugHook::online` returns nothing. A CPU online without a working device ends no slices and runs no timers, and its heartbeat stops, so the lockup detector panics the machine about a CPU the bring-up could have refused. Fix: hooks that answer a `Status`. A failing online hook unwinds the levels already brought up and leaves the CPU offline, and `bring_cpu_online()` returns the failure.

## Minor and style
- **Naming:**
  - One act has three names across the layers: `RunQueue::arm_preempt_timer()`/`disarm_preempt_timer()`, `SchedClock::reset_preempt()`/`cancel_preempt()`, `TimerQueue::reset_preempt()`. None is a timer any more; all set or clear the CPU's preemption deadline. One vocabulary, such as `set_preemption_deadline()` and `clear_preemption_deadline()`, would say what the slot is at every call.
  - `TickSched` (`kernel/include/ours/time/tick_sched.hpp:41`) now holds a canary, `m_started` and `on_tick()` for the periodic tick: nothing of a scheduler and little of a tick. Whether it keeps its name, or folds into `TickDevice::handle_periodic_event()`, is a question for the user, not a deletion.
- **Style, in this work's own lines:**
  - Ternary operators, which the style forbids: 15. Among them `kernel/arch/x86/lib/idle_states/src/idle_states.cpp:83`, `kernel/kernel/task/idle_thread.cpp:129`, `kernel/include/ours/task/sched/sched_entity.hpp:402`, `kernel/arch/x86/kernel/task/idle_thread.cpp:93` and `:127`, `kernel/commands/idle.cpp:88`, `:95` and `:146`, `kernel/commands/preempt_test.cpp:225`, `:228`, `:277`, `:278` and `:288`, `kernel/commands/hotplug_test.cpp:383`, `benches/kernel/time/preempt_reset_bench.cpp:203`.
  - A `[[nodiscard]]` result discarded with `static_cast<void>`, where the rule is `auto _ = ...` with a comment saying why: `kernel/kernel/task/sched/scheduler.cpp:711`, the line of 3449895f.
  - An unused parameter cast to `void` instead of `[[maybe_unused]]`: `kernel/kernel/task/cpu_hotplug.cpp:61`. The same for an unused local: `benches/kernel/time/preempt_reset_bench.cpp:226`.
  - An unscoped `enum MonitorState : u32` (`kernel/arch/x86/kernel/task/idle_thread.cpp:22`), where the rule is `enum class`.
- **A hook with no caller:** `ISchedule::update_thread()` (`kernel/include/ours/task/sched/schedule.hpp:133`) has none since c6764d82 removed the tick's call. Fair's override is now dead code. Fair is the user's, so this is theirs to decide.
- **The fallback path's cost:** a CPU served by a broadcast group takes the group's shared lock at every pick, through `reprogram_locked()` and `TickBroadcast::set_deadline()` (`kernel/kernel/time/tick_broadcast.cpp:124`). That cost exists only while a local device has failed. It is a performance note for `docs/repairs/`, not a defect.

## Teaching opportunities
### T1. Consume a request where it is satisfied, not where it is next noticed
`kernel/kernel/task/sched/scheduler.cpp:711`. The invariant: a local pending preemption means "this CPU owes a reschedule that has not happened yet". Who writes it: a waker on the same CPU (`reschedule_mask()`), the preemption deadline (`on_preempt_timer()`), a reschedule IPI. Who reads it: the next preemption point (`enable_preemption()`, an interrupt's exit). The worst interleaving was the ordinary one:
1. A posts B's semaphore, and the request goes on A.
2. A blocks, and the reschedule switches to B: the debt is paid.
3. B blocks, and the reschedule switches back to A.
4. A's next preemption point finds the request still set and reschedules to pick A again.

A flag that outlives the event it asks for is a stale flag. The fix clears it at the one place the debt is paid, which is what Zircon's `Scheduler::RescheduleCommon()` does (`preempts_pending_clear()` after `EvaluateNextThread()`). With a different scheme, say a per-CPU flag instead of a per-thread one, the same rule would hold: whoever performs the reschedule retires the request.

### T2. Deferring work out from under a lock needs a place where it is sure to happen
M2. `reset_preempt()` cannot run `notify_refusal()` under the run queue lock: the answer swaps devices, takes the broadcast group's lock, and logs, any of which could wake a thread and need the run queue lock again on this CPU. So it records the refusal and leaves. Recording is right; the question is who acts on the record. The design leaned on "the queue's next visit", which the tick used to guarantee within 10 ms, and tickless idle took that guarantee away without anyone noticing. The general pattern is to record under the lock and act at the first point after release that every path passes through. In the scheduler that point exists: the end of a reschedule. Linux has the same concern in `hrtimer_interrupt()`: when it detects that it cannot keep up (`hang_detected`), it still programs the next event, a little later, rather than leave the device to whatever comes next.

### T3. A single writer turns "who is right about the device" into one function
`kernel/kernel/time/timer_queue.cpp:71`. Before the slot, the scheduler and the queue both wrote the device, each with its own idea of what it held, and the later write won: a timer could fire 59 ms late with nothing to say so. With the slot, `earliest_deadline_locked()` is the whole policy: the timers, held off; the slice, not held off; the earliest of those. `m_armed_deadline` is a cache of the device that can be trusted, because nothing else writes it. Another session's hold-off then dropped in without touching the scheduler at all, which is the payoff of a single writer: new policy is one more term in one function. Zircon's `TimerQueue::UpdatePlatformTimerLocked()` has the same shape.

## Suggested follow-up order
1. Naming: one vocabulary for setting and clearing the preemption deadline; the user's decision on `TickSched`.
2. Style sweep of this work's lines: the ternaries, the two `void` casts, `enum class MonitorState`.
3. M2: `notify_refusal()` where the scheduler drops the run queue lock. Small, and it restores a bound the tick used to provide.
4. M1 and M3 together, since both are about a hotplug wait that can fail: bounded waits, a rollback to `{Offline, Offline}`, Dpc workers started from the CPU's side.
5. M4: online hooks that answer a `Status`, with the levels unwound on failure.
6. Questions for the user: `ISchedule::update_thread()` and Fair; the broadcast-group lock as a repair item.
