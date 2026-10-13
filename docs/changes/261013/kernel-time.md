# kernel-time changes

## A slice ends at its CPU's preemption deadline, a slot in the timer queue, not on a tick (2026-10-13)

**Files**
- `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp` — the preemption-deadline slot (`m_preempt_deadline`, `reset_preempt()`), folded into what the device is armed for and taken first by `run_expired_timers()`; the counter `timer.queue.preempts` (`num_preempts()`)
- `kernel/include/ours/task/sched/scheduler_inline.hpp` — `Scheduler::set_next_thread()` sets the deadline at every pick, and empties it for the idle thread
- `kernel/include/ours/task/sched/sched_entity.hpp` — `SchedEntity::remaining_slice()`
- `kernel/include/ours/task/sched/sched_clock.hpp`, `kernel/kernel/task/sched/sched_clock.cpp` — `reset_preempt()` writes the slot instead of the device; `cancel_preempt()`
- `kernel/include/ours/task/sched/run_queue.hpp` — `disarm_preempt_timer()`; comments
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp` — `Scheduler::Current::on_preempt_timer()` replaces `Current::tick()` and `tick_thread()`
- `kernel/include/ours/task/sched/rr.hpp`, `kernel/kernel/task/sched/rr.cpp` — `update_thread()` and `slice_spent()` go
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp` — the tick no longer calls the scheduler
- `kernel/commands/preempt_test.cpp`, `kernel/commands/BUILD.gn` — `preempt.test`
- `docs/repairs/261009/kernel-time.md` — item 2 fixed, the file removed with its last item
- `docs/repairs/261013/kernel-time.md` — the cost on the switch path, as a performance item
- `.agents/skills/ours-kernel-design/references/task.md` — the decision

**Symptom**
None, a step of the idle-power task (step 3 of `.agents/tasks/idle-power/PLAN.md`). The round-robin slice ended only when a tick found it spent: every 10 ms `TickSched::on_tick()` called `Scheduler::Current::tick()`, which charged the running thread and, if its slice was spent and another thread was queued, asked for a reschedule. So a 64 ms slice ended at the first tick past 64 ms, 70 ms after it began, and no CPU could do without the tick while the scheduler needed it.

Separately, `SchedClock::reset_preempt()`, Fair's way of setting a preemption deadline, wrote the tick device behind the timer queue's back (`docs/repairs/261009/kernel-time.md` item 2).

**Cause**
The tick sampled a condition that has a known time. A slice that starts at T ends at T plus its length, and that is known the moment the thread is picked; checking it every 10 ms costs 100 interrupts a second on every CPU and still ends the slice up to a tick late. With the device one-shot, it can be armed for the slice's end directly.

It cannot be armed by the scheduler writing the device, though. The timer queue arms the device for its earliest timer and remembers what it armed (`m_armed_deadline`), so that it does not write the device again for the same deadline. A second writer breaks that record: the scheduler arms the device for a slice ending in 64 ms, the queue still believes it is armed for a timer due in 5 ms, and that timer fires 59 ms late with nothing to say so. That was the latent bug in repair item 2, and why `SchedClock::reset_preempt()` carried an assertion that the device was periodic. There must be one writer, and the queue is the one that knows the timers, so the deadline becomes something the queue holds.

**Change**
The queue holds one deadline that is not a timer:

```cpp
/// When the running thread's slice ends, or INFINITE_TIME while this CPU
/// runs its idle thread. ...
TimePoint<> m_preempt_deadline TA_GUARDED(m_lock);
```

What the device is armed for is the earlier of it and the timers. Before:

```cpp
auto TimerQueue::earliest_deadline_locked() const -> TimePoint<> {
    TimePoint<> deadline = INFINITE_TIME;
    for (auto const &base: m_bases) {
```

After:

```cpp
auto TimerQueue::earliest_deadline_locked() const -> TimePoint<> {
    TimePoint<> deadline = m_preempt_deadline;
    for (auto const &base: m_bases) {
```

`reset_preempt(deadline)` writes the slot under the lock and calls `reprogram_locked()`. `run_expired_timers()` takes a passed deadline first, in the critical section that already clears `m_armed_deadline`, empties the slot, and after dropping the lock tells the scheduler:

```cpp
slice_over = now >= m_preempt_deadline;
if (slice_over) {
    m_preempt_deadline = INFINITE_TIME;
}
...
if (slice_over) {
    TIMER_QUEUE_PREEMPTS.add(1);
    task::Scheduler::Current::on_preempt_timer();
}
```

The slot is emptied there, not left for the scheduler. A slot still in the past would arm the device for "now" at the end of the pass, and again after every interrupt, until the reschedule set a new one.

`on_preempt_timer()` only asks for a reschedule (`preemption_state().add_pending()`), which is paid at the interrupt's exit, as Zircon's `Scheduler::TimerTick()` does. The reschedule does the rest: `put_prev_thread()` charges the thread and puts it at the back of the FIFO, and the next pick sets the next deadline.

The deadline is set at every pick, in `Scheduler::set_next_thread()`, after the discipline has started the slice:

```cpp
ctx.schedule()->set_next_thread(m_run_queue, next);

if (next == m_idle_thread) {
    m_run_queue.disarm_preempt_timer();
} else {
    m_run_queue.arm_preempt_timer(ctx.remaining_slice());
}
```

Three things make this the right place:
- every way a thread comes to run goes through it, the running thread picked again included. `evaluate_next_thread()` calls `put_prev_thread()` and `set_next_thread()` even when the answer is the current thread, so a lone runnable thread whose slice ends gets a new slice and a new deadline without a special case;
- the idle thread has no slice, so its CPU is woken by its timers alone; that is the point of the whole task;
- the scheduler, not each discipline, decides that the idle thread has none. A discipline only says how long the slice is, through the framework's own accounting (`m_timeslice`, `m_exec_runtime`, `m_prev_exec_runtime`), read by the new `SchedEntity::remaining_slice()`.

`SchedClock::reset_preempt(delta)` keeps its signature, so Fair's call through `RunQueue::arm_preempt_timer()` is untouched, and now writes the slot (`TimerQueue::get().reset_preempt(...)`) instead of the device.

What the tick did is gone: `TickSched::on_tick()` no longer calls the scheduler, and `Scheduler::Current::tick()`, `Scheduler::tick_thread()`, `RoundRobin::update_thread()` and `RoundRobin::slice_spent()` go with it. Charging happens where it always also did, when a thread is put back. With a periodic device, which cannot be armed for a deadline, `on_tick()` still runs the timer queue every tick, and the queue finds a passed deadline there, so a slice ends at the first tick past its end, as before.

Rejected:
- the discipline arms the deadline in its own `set_next_thread()`, as Fair already does under `sched_nohz`. That needs every discipline to also know not to arm for the idle thread, which is a scheduler fact, and two writers per idle entry otherwise;
- leave the slot empty for a lone runner once its slice ends, since nobody is waiting, and preempt it when a thread arrives. That is fewer interrupts on a CPU with one busy thread, but it breaks Zircon's rule that only the idle thread runs with no deadline set. Then any path that queues a thread without a reschedule IPI would leave the newcomer waiting behind a thread that is never stopped.

The lock order is new: the scheduler holds its run queue lock in `set_next_thread()` and takes the timer queue's inside it. Nothing takes them the other way: a timer's handler runs with the queue lock dropped, and `on_preempt_timer()` is called after it is dropped. For the same reason `reset_preempt()` does not pass on a device refusal (`notify_refusal()`), whose answer takes locks of its own; the flag stays for the next visit to the queue.

**Reference**
Zircon `TimerQueue::PreemptReset()`, `TimerQueue::UpdatePlatformTimerLocked()` and `TimerQueue::Tick()` (`zircon/kernel/kernel/timer.cc`): the preemption deadline is a field of the per-CPU timer queue, the platform timer is armed for the earliest of it and the timer lists, and a passed deadline is taken before the timers and handed to `Scheduler::TimerTick()`, which only sets a preemption pending. Zircon `Scheduler::RescheduleCommon()` (`zircon/kernel/kernel/scheduler.cc`) sets it on every reschedule, idle thread excepted, and asserts that nothing but the idle thread runs with it unset.

**Verification**
- Built in `.worktrees/wip-proc-self` at 7a5e86db plus only this change; `out/hotplug-off` (no `cpu_hotplug`) and `out/hotplug-periodic` (`time_oneshot` off) build too. clang-tidy reports nothing on the changed lines of the `.cpp` files.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed.
- `preempt.test` on dbg, three runs: ok, with 12 to 13 turns a second, all of them a whole slice, the longest 65.2 to 65.7 ms against a 64 ms slice; the probe timer 1.2 ms late (timer.test, unchanged, puts a lone 1 ms timer 0.3 to 1.3 ms late here); no idle CPU passed a deadline.
- `preempt.test` on the periodic build: ok, turns 70.3 ms at the longest, as the tick rounds them up.
- `hotplug.test`, `idle.test`, `sleep.test`, `timer.test`, `tickbcast.test`, `tickreplace.test`, `timershare.test`, `dpc.test`, `semaphore.test`, `waitqueue.test`, `threadjoin.test`, `samplerclaim.test` on dbg: all ok; `hotplug.test`, `sleep.test`, `timer.test` on the periodic build: ok.
- Mutations, each caught by `preempt.test`:
  - `set_next_thread()` never arming: 0 turns, "the spinners take turns" fails;
  - the idle thread armed like any other: CPUs 0 to 2 pass 14 to 16 deadlines a second, "no other CPU passes a preemption deadline" fails;
  - `earliest_deadline_locked()` answering the slot alone when it is set: the probe fires 13 ms late, "a timer due before the slice ends fires on time" fails.
- `idle.stats 2` on an idle machine: about 101 timer interrupts a second per CPU, unchanged, because the tick `Timer` is still armed (step 4 removes it).

**Deliberately not done**
- The tick `Timer` stays, now only for jiffies; removing it is the next step, introduced separately.
- Fair is out of the build and untouched. It loses the per-tick `update_thread()` call, which has no caller now; when it comes back, the scheduler's deadline at each pick applies to it as to RR.
- The cost on the switch path, a lock and a device write per pick, is recorded as `docs/repairs/261013/kernel-time.md` item 1, not optimised.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must the scheduler's deadline live in the timer queue rather than be written to the device by the scheduler itself? Give the timeline in which a timer fires late.
2. Why does `run_expired_timers()` empty the slot itself instead of waiting for the reschedule to overwrite it?
3. A lone runnable thread's slice ends. Follow it from the device interrupt to the next deadline being set: which functions run, and why is no special case needed for "nobody else to run"?
