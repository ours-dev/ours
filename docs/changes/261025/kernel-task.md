# kernel-task changes

## A reschedule consumes the request for it, so a hand-off is not followed by a second one (2026-10-25)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` — `reschedule_common()` takes the running thread's local preemption request once the next thread is chosen
- `kernel/commands/preempt_test.cpp` — `check_hand_off()`: a hand-off of 400 switches sets the deadline at most 440 times
- `docs/repairs/261025/kernel-task.md` — item 1 fixed, the file removed with it
- `docs/repairs/261013/kernel-time.md` — item 1's counts updated

**Symptom**
`preemptreset.bench`: two threads handing a token back and forth with semaphores on one CPU set its preemption deadline and wrote its timer device 2.00 times per switch. A probe in `reschedule_common()` counted 4,002 switches and, beside them, 3,997 reschedules in `Mode::Preempt` that picked the thread already running.

**Cause**
A thread that wakes another on its own CPU records a local preemption request on itself (`Scheduler::reschedule_mask()` calls `PreemptionState::add_pending()`): its CPU should reschedule at the next chance. In a hand-off the waker blocks right after, and its reschedule switches to the thread it woke, which is what the request asked for. But `reschedule_common()` only sent the remote requests on (`flush_pending_remote()`) and left the local one set, on the waker. When the waker was switched back in, its next preemption point (`enable_preemption()` and then `flush_pending_local()`) found the request and rescheduled again. With nothing else to run, that reschedule picked the waker again, and still set the deadline and wrote the device. A request that outlives the event it asked for is a stale flag. The fix is to consume it where it is satisfied, not where it is next noticed.

**Change**
After `evaluate_next_thread()`, whichever thread it chose:

```cpp
static_cast<void>(curr->preemption_state().take_pending_local());
```

Interrupts are off from there to the switch, so no new request can arrive in between and be lost. `take_pending_local()` clears both the CPU's bit and the request flag, the two halves its own comment says must go together.

`preempt.test` gains `check_hand_off()`: 200 round trips between two threads on the test's CPU, counting `TimerQueue::num_preempt_resets()`, which must stay within a tenth of one per switch.

**Reference**
Zircon `Scheduler::RescheduleCommon()` (`zircon/kernel/kernel/scheduler.cc`): after `EvaluateNextThread()`, `mp_reschedule(preempts_pending())` then `preempts_pending_clear()`, the local bit included.

**Verification**
- Built in `.worktrees/wip-proc-self` at 5ff0d2a1 plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too. clang-tidy reports nothing on the changed lines.
- `boot_check.py`: dbg and rel PASS, host tests 146 passed.
- `preemptreset.bench all`: 1.00 deadline sets and device writes per switch, down from 2.00; a switch 9.1 us under TCG, down from 17.6 us (not a hardware number).
- `preempt.test` twice: the hand-off set the deadline 400 times for 400 switches. `hotplug.test`, `semaphore.test`, `waitqueue.test`, `threadjoin.test`, `sleep.test`, `timer.test`, `dpc.test`, `idle.test`, `timershare.test`, `tickbcast.test`, `samplerclaim.test` and `mutex.stress` (20,000 rounds × 3 threads) ok; on the periodic build `preempt.test`, `semaphore.test`, `hotplug.test` ok.
- Mutation: without the line, the hand-off sets the deadline 800 times and `check_hand_off()` fails.

**Deliberately not done**
The candidates of `docs/repairs/261013/kernel-time.md` item 1 (fewer clock reads, the device written lazily): the user's to choose next.

**How it was found**
`preemptreset.bench` counted 2 deadline sets per switch where the code's reading said 1. A temporary pair of counters in `reschedule_common()`, for picks that kept the thread and picks that switched, showed the extra half were all `Mode::Preempt` and all kept the thread.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. In a hand-off, who sets the local preemption request, and which event satisfies it?
2. Why is it safe to clear the request in `reschedule_common()`, and what would make it unsafe?
3. Why did the stale request cost a device write, and not only a trip through the scheduler?

## The preemption deadline is built from the clock's last reading, not a new one (2026-10-25)

**Files**
- `kernel/kernel/task/sched/sched_clock.cpp`, `kernel/include/ours/task/sched/sched_clock.hpp` — `SchedClock::reset_preempt()` adds the slice to `m_prev_real_clock`
- `kernel/include/ours/task/sched/run_queue.hpp` — `arm_preempt_timer()`'s comment
- `docs/perf/261025/pick-cost.md`, `docs/perf/261025/pick-cost/stage-2.csv` — stage 2 of the report
- `docs/repairs/261013/kernel-time.md` — item 1's candidates

**Symptom**
Each pick read the steady clock in `SchedClock::reset_preempt()` to build the deadline, a few hundred nanoseconds after the reschedule had read it on its way in (`Scheduler::reschedule_common()` updates the scheduler's clock first). The clock is the HPET, whose reads are slow. A probe counted 5.00 clock reads per switch on the hand-off's CPU.

**Cause**
`reset_preempt()` asked for "|delta| from now" as if called out of nowhere, but its only caller is a pick, inside a reschedule that has just taken the time. The second read added nothing but its cost and the few hundred nanoseconds it moved the deadline.

**Change**
Before:

```cpp
auto const deadline = to_sched_time(SteadyClock::now()) + delta;
```

After:

```cpp
auto const deadline = m_prev_real_clock + delta;
```

`m_prev_real_clock` is the clock's last real reading, the one `update_clock()` takes. When the reschedule's update was skipped (`ActSkip`), it is the reading of the enqueue that asked for the skip, taken moments earlier, and the slice ends that much sooner.

The read inside the device's programming (`ClockEventDevice::program_event()`) stays: removing it needs the time passed down through three timer layers.

**Reference**
Zircon's `PreemptReset()` (`zircon/kernel/kernel/scheduler.cc`) takes the `now` latched when the CPU entered the scheduler, and reads no clock of its own.

**Verification**
- Built in `.worktrees/wip-proc-self` at 2d348e92 plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too. clang-tidy reports nothing on the changed lines.
- `boot_check.py`: dbg and rel PASS, host tests 146 passed.
- `preempt.test`: whole slices, the longest 66.3 ms on one-shot and 70.3 ms on the periodic build. `hotplug.test`, `semaphore.test`, `waitqueue.test`, `threadjoin.test`, `sleep.test`, `timer.test`, `dpc.test`, `idle.test`, `timershare.test`, `mutex.stress` ok.
- A probe, not kept, counting `SteadyClock::now()` on the hand-off's CPU: 5.00 reads per switch before, 4.00 after.
- `docs/perf/261025/pick-cost.md` stage 2: under TCG, the hand-off's median 10.5 to 8.8 us per switch, inside the emulator's spread on its own, outside it against the baseline.

**Deliberately not done**
The device's own read, as above; the other candidates of `docs/repairs/261013/kernel-time.md` item 1.

**How it was found**
Reading the pick's path for the measurement method of the performance item.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Where does a pick's first clock read happen, and why is a second one there worth nothing?
2. What is `m_prev_real_clock` after a skipped update, and what does that do to the slice?
3. Why is the read inside the device's programming harder to remove than this one?
