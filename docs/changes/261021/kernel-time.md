# kernel-time changes

## The fallback to the periodic tick has a test, and is a warning (2026-10-21)

**Files**
- `kernel/commands/tick_fallback_test.cpp` — `tickfallback.test`; `kernel/commands/BUILD.gn` lists it
- `kernel/kernel/time/tick_device.cpp` — the log line of the fallback in `rebind_device()` is a warning, not an error
- `docs/repairs/261010/kernel-time.md` — item 3 is removed, as fixed
- `.agents/tasks/high-res-timers/STATUS.md` — the open list

**Symptom**
Latent, cannot be triggered with the devices in the tree: the local APIC timer always accepts one-shot, so the branch of `rebind_device()` that handles a device that claims Oneshot and will not enter it never ran. Nothing said that a CPU in that case keeps a tick, or that `m_mode` stays Periodic.

**Cause**
The branch needs a device that advertises `Feature::Oneshot` and fails `switch_mode(Oneshot)`, and no real device does. A path that has never run is a path whose order of operations nobody has looked at: the one that matters here is `setup_oneshot_tick()`, which sets the handler, asks the device for Oneshot, and only then writes `m_mode = Oneshot`. Writing the mode first would leave the CPU believing it is one-shot while its device runs a periodic tick or none, and the timer queue would then stop being run from `on_tick()`, which follows the mode. A CPU in that state has no timers at all.

The line that reports the fallback was also at error level, and `scripts/kshell_run.py` fails a command that prints an error line, so no test of the fallback could pass. The fallback is the CPU carrying on with a coarser tick; it is a warning.

**Change**
`tickfallback.test` builds a `TickDevice` of its own on the stack and offers it a fake device that refuses Oneshot (and runs Periodic):

```cpp
TickDevice own;
offered = own.check_new_device(&fake.device());
still_periodic = own.mode() == TickDevice::Mode::Periodic;
```

It is safe on a running kernel for three reasons. A fresh `TickDevice` points at the placeholder device and starts Periodic, so the first real device is taken. The refusal returns from `setup_oneshot_tick()` before the timer queue or the tick is touched, and the periodic setup that follows touches only the fake. And the fake is not enrolled: `ClockEventDevice::enroll()` would offer it to the CPU's own `TickDevice`, which would take it, rated as it is above the local APIC timer.

Checks: the device is taken (`Ok`); the tick device points at it; the mode is still Periodic; the device runs Periodic with a handler on it; it was asked for Oneshot once. With `kernel.time.oneshot` off it prints a skip line and passes, since the fallback is then never entered.

**Reference**
Linux `tick_switch_to_oneshot()` (`kernel/time/tick-oneshot.c`) refuses before it changes anything when the device lacks `CLOCK_EVT_FEAT_ONESHOT` or is not functional (`-EINVAL`, mode untouched). It then writes `td->mode` before `clockevents_switch_state()` and does not look at the result of the switch. So Linux decides on what the device claims; this tree decides on what the device answered, and commits the mode after it, which is stricter and is what the test holds to. `docs/repairs/261010/kernel-time.md` had cited Linux for the stricter behaviour; it was the capability check only.

**Verification**
- `tickfallback.test` on the default build (one-shot): 6 checks ok. The log shows `could not run oneshot-refusing one-shot (NotSupported), keeping the periodic tick` as a warning.
- Mutation, not committed: `m_mode = Mode::Oneshot` moved above `switch_mode(Oneshot)` in `setup_oneshot_tick()`. The test fails exactly the check "the mode is still Periodic" (1 of 6) and the command reports FAILED; with the line restored it passes again. This is the acceptance of the repair item: the test fails if the mode is committed before the device accepted.
- On a build with `time_oneshot=false`: the skip line, ok. Both images of that build boot.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (145). `tickreplace.test`, `tickbcast.test`, `timer.test` and `sleep.test`: ok.
- `scripts/format_cpp.py`, `scripts/format_gn.py`, `clang-tidy -p out/bringup.x86` on the new file and `tick_device.cpp`: no diagnostic in them.
- Not verified: a real device that refuses Oneshot, and the CPU that has no device at all (item 6).

**Deliberately not done**
- A case where the device also refuses Periodic. `rebind_device()` then returns the error and leaves the CPU bound to a device that is not in service; whether that should be handled is item 6's question, a design decision for the user.
- Offering the fake to the CPU's real `TickDevice`. It would exercise `replace_with()`, which `tickreplace.test` does.

**How it was found**
Picked from the open repairs: a path with no test and a known order-of-operations hazard. The error-level line was found by running the test.

**The user's prediction**
Not given.

**Three questions you should be able to answer**
1. Why does writing `m_mode` before the device accepts Oneshot leave the CPU without timers, and not only with a wrong tick?
2. Why is the test's `TickDevice` safe to use on a running CPU, and what would go wrong if the fake were enrolled?
3. What does Linux's `tick_switch_to_oneshot()` check, and what does it not?

## A timer pass that ran callbacks holds the device off for its own length (2026-10-21)

**Files**
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp` — `m_hold_off_until`, set at the end of `run_expired_timers()` and applied in `earliest_deadline_locked()`; two counters, `timer.queue.held_passes` and `timer.queue.held_off_ns`, with `num_held_passes(cpu)` and `held_off_ns(cpu)`
- `kernel/commands/timer_test.cpp` — a check that a slow pass holds the next interrupt off; `kernel/commands/timer_share_test.cpp` — `timershare.test big` now asserts, and prints the average hold-off
- `docs/architecture/timer/callback-cpu-share.md`, `docs/repairs/261010/kernel-time.md` (item 5 removed), `.agents/tasks/high-res-timers/DESIGN.md`, `STATUS.md`, `.agents/skills/ours-kernel-design/references/task.md`

**Symptom**
`timershare.test big` (2026-10-21, earlier entry): with a 10 µs periodic timer, the interrupted thread kept 29 %, 12 %, 6 % and 4 % of its CPU at callbacks of 0.5, 1, 2 and 4 ms. Under QEMU TCG; on a device that interrupts in microseconds the same shares come at callbacks of tens of microseconds.

**Cause**
A periodic timer restarts from its callback with `forward()`, which puts the next deadline on the first multiple of the period after the callback ended: within one period of now. The queue arms the device for it and the device interrupts again as soon as it can. Nothing connects how long the pass took to how soon the next one may come, so the thread's share is $g / (C + g)$ with $g$ the device's delivery time, whatever $C$ is.

**Change**
The user chose, on 2026-10-21, to hold the whole device off after a long pass. At the end of a pass that ran at least one callback, the queue records when the device may next be armed for a timer, the end of the pass plus the pass's own length, at most 100 ms:

```cpp
auto const end = SteadyClock::now();
auto const held = std::min(end - now, MAX_HOLD_OFF);
hold_off_until = end + held;
...
m_hold_off_until = hold_off_until;     // under the lock, before reprogram_locked()
```

and `earliest_deadline_locked()` takes the later of the earliest timer and that time:

```cpp
deadline = std::max(deadline, m_hold_off_until);
return std::min(deadline, m_preempt_deadline);
```

So $g \ge C$ and the thread keeps at least $C / (C + C) = 1/2$ of the CPU while the pass lasts ($\lambda = 1$). The preemption deadline, the end of the running thread's slice, is not held off: it is the thread's own time, one interrupt per slice is no storm, and a held-off slice end would let a thread overrun its slice by as long as the pass. `INFINITE_TIME` stays infinite, since nothing is later. A pass that ran nothing leaves an earlier hold-off as it is, so a preemption interrupt in the middle of one does not lift it. Everything that arms the device goes through `reprogram_locked()` (activation, cancellation, `rearm_device()`, `try_rearm_device()`, the broadcast path), so it is all covered, and `armed_deadline()`, which the idle governor reads, now names when the device will really fire.

The trigger differs from Linux's, on purpose. `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) holds the device off only when, after a pass, the next expiry is already in the past (`expires_next < now`, after three retries), and then arms `ktime_get() + min(100 ms, max_hang_time)` (`hrtimer_rearm()`); the delay is the largest hang seen on the CPU, not this pass's. A periodic timer that restarts after a slow callback is ahead of now, by up to its period, so that trigger would never fire for it. Here every pass that ran a callback holds the device off, for its own length, and the 100 ms cap is Linux's.

**Cost, said plainly**
- Another timer is late by up to the length of the pass before it: the price of holding the whole device. `timer.test` states it: after a 2 ms pass, a timer that fell due during it ran 2.5 ms later at the soonest.
- One more clock read per pass that ran a callback: an HPET read, about 8 µs under TCG. The pass already reads the clock at entry, and `ClockEventDevice::program_event()` reads it again. Not measurable in the lateness of 1 ms timers: `timer.test` medians were 294, 312 and 318 µs before and 330, 381, 294 and 302 µs after, with the host's noise at about 40 µs.
- The tick pass (about 25 µs, every 10 ms) holds the device off for about as long, which is a few tens of microseconds of lateness for a timer due just after it.

**Reference**
Linux `hrtimer_interrupt()` and `hrtimer_rearm()` (`kernel/time/hrtimer.c`), as above, for the cap and the idea; the trigger and the length are this project's. Zircon has no counterpart: its timers do not restart themselves, so a slow callback cannot make itself due again.

**Verification**
- `timershare.test big` (medians of five paired repeats; before, this session, then after):

  ```text
  C asked   before   after   hold-off (average)   gap left to the thread
  0.5 ms    28.8 %   69.1 %   507 us               1147 us
  1 ms      11.9 %   62.8 %  1006 us               1770 us
  2 ms       6.3 %   55.8 %  2063 us               2882 us
  4 ms       4.3 %   55.1 %  4012 us               4706 us
  ```

  The average hold-off is the pass's own length, as designed. The gap is that plus what the emulated timer adds, 0.6 to 1.1 ms, which is the delivery of an interrupt armed a little ahead, where one armed for a deadline already past is delivered at once; on a real device it is microseconds. The default `timershare.test` points (20, 45, 100 µs) went from 86 %, 71 %, 63 % to 97 %, 95 %, 91 %. `big` now asserts at least 45 % at each point.
- Mutation, not committed: the hold-off computed and counted and never applied. `timer.test` fails exactly the check "it came no sooner than about the pass's own length" (the next interrupt came 18 µs after a 2000 µs pass, against 2269 to 2528 µs with the hold-off); `timershare.test big` fails at all four points.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (145). `preempt.test` (the preemption slot), `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test`, `waitqueue.test`, `tickreplace.test`, `tickbcast.test`, `tickfallback.test` and `mutex.stress 2000`: ok. On a `time_oneshot=false` build both images boot and `timer.test` (the hold-off check skipped), `sleep.test` and `preempt.test` are ok.
- `scripts/format_cpp.py` and `clang-tidy -p out/bringup.x86` on the touched sources: no diagnostic in them.
- Not measured: a real machine's delivery time and the callbacks that run there; the cost of the extra clock read on real hardware; anything under load beyond these tests.

**Deliberately not done**
- Penalising only the timer whose callback is slow (option B in `STATUS.md`): it would leave the other timers alone and lose the slow one's phase. A was chosen.
- A threshold below which a pass holds nothing off: it would add a tuning number, and the hold-off of a short pass is as short.
- Holding off the preemption deadline.

**How it was found**
`timershare.test big` and the question of what a guard would have to do (earlier entry). A first reading of the numbers took the extra gap between callbacks as a defect of the guard; the counters added here show the hold-off itself equals the pass length, and the rest is the emulator's.

**The user's prediction**
Not given.

**Three questions you should be able to answer**
1. Why does the thread keep at least half the CPU, and what makes it more than half here?
2. Why would a trigger of "the next timer is already late" never fire for a periodic timer with a slow callback?
3. Why is the preemption deadline left out of the hold-off, and what would happen to a slice if it were not?
