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
