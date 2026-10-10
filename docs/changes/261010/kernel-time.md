# kernel-time changes

## TimerQueue stops recording a deadline the tick device refused (2026-10-10)

**Files**
- `kernel/kernel/time/timer_queue.cpp` — `reprogram_locked()` records the armed deadline only when `program_event()` succeeded; a refusal is counted in `timer.queue.program_refused` and leaves the armed deadline unknown
- `kernel/include/ours/time/timer_queue.hpp` — `m_next_deadline` becomes `m_armed_deadline`, and `UNKNOWN_DEADLINE` names "the device may hold anything"
- `docs/repairs/261009/kernel-time.md` — item 1 is replaced by the missing test of this path
- `.agents/tasks/high-res-timers/DESIGN.md` — the "What is missing" bullet for this bug is marked fixed

**Symptom**
Latent, cannot be triggered today. Every CPU's tick device runs Periodic, where `TickDevice::program_event()` returns Ok without touching the hardware, so the cache could not be wrong. On a CPU that runs one-shot, a `program_event()` that fails (the HPET does, when `program_min_delta()` runs out of attempts and returns TimeOut) left the device unarmed while the queue believed it was armed for that deadline. The next `activate_timer()` whose earliest deadline was the same returned early from `reprogram_locked()`, nothing armed the device, and this CPU's timers did not fire until a different earliest deadline appeared.

**Cause**
`m_next_deadline` is a cache of the device's state, and a cache may change only when the thing it mirrors has changed. `reprogram_locked()` wrote it on the line after the call, whatever the call returned, and `auto _ =` discarded the result in the same breath. In Periodic mode that was harmless: the call is a no-op that reports success, and the tick runs the queue anyway, so a wrong cache cost nothing. The comment on that line said as much. The code was right for the mode it ran in and wrong for the mode it was written for.

Why a wrong cache loses the timer, and not merely delays it: in one-shot mode the only thing that runs `run_expired_timers()` is the device interrupt, and the device interrupts only when armed. The early return `deadline == cache` is the single place that decides "the device already has this", so a cache that says yes when the answer is no removes the only retry. The failure is also asymmetric. A cache that forgets (says "unknown" when the device is armed) costs one redundant `program_event()`. A cache that remembers wrongly (says "armed" when the device is not) costs a wakeup that never happens.

**Change**
Before:

```cpp
auto _ = TickDevice::Current::get().program_event(deadline);
m_next_deadline = deadline;
```

After:

```cpp
auto const status = TickDevice::Current::get().program_event(deadline);
if (Status::Ok != status) {
    m_armed_deadline = UNKNOWN_DEADLINE;
    TIMER_QUEUE_PROGRAM_REFUSED.add(1);
    return;
}
m_armed_deadline = deadline;
```

and the early return became `if (deadline == m_armed_deadline)`, so `UNKNOWN_DEADLINE` never equals a real earliest deadline and the next reprogram always tries again.

Ruled out:
- Asserting success. A deadline the device cannot reach is an expected condition, not a bug.
- `ustl::Option<TimePoint<>>`, "none" for unknown. It was tried first and does not compile: `Option`'s copy and move assignment require a nothrow-assignable payload, and `ustl::chrono::TimePoint` is not declared so. The sentinel is the same idiom `INFINITE_TIME` already uses at the other end of the range. The residual case: a timer whose deadline is exactly `TimePoint<>::min()` would have one reprogram skipped after a refusal.

**Reference**
Neither reference does this, so it is this project's own design. Linux `__hrtimer_reprogram()` (`kernel/time/hrtimer.c`) writes `cpu_base->expires_next` first and calls `tick_program_event(expires_next, 1)` after, ignoring the result; it does not need the cache to track a refusal because `force=1` makes `clockevents_program_event()` clamp a close deadline to the minimum delta. Zircon `TimerQueue::UpdatePlatformTimerLocked()` (`zircon/kernel/kernel/timer.cc`) calls `platform_set_oneshot_timer()`, which returns a `zx_status_t`, and sets `next_timer_deadline_` right after without reading it. Here the device layer can say TimeOut even after its own retries, so the cache has to follow the outcome.

The Reference of the repair item this replaces claimed that Linux `hrtimer_reprogram()` leaves `expires_next` to `tick_program_event()`'s outcome. That is wrong, and is corrected above. Linux's `retry:` loop in `hrtimer_interrupt()` is hang detection for timers that expire again during the callbacks, not handling of a refusal.

**Verification**
- `scripts/format_cpp.py --check` on both files: clean.
- `clang-tidy -p out/bringup.x86 kernel/kernel/time/timer_queue.cpp`: 38 diagnostics in the translation unit, none on a line of `timer_queue.cpp` or `timer_queue.hpp`.
- `ours-verify` (`boot_check.py`): dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (144 passed).
- The refusal path itself was never executed: not measured. Periodic mode cannot produce a refusal, so only reading the code and the boots above back the claim that behaviour is unchanged where it runs today.

**Deliberately not done**
- The test. The acceptance of the repair asked for a device that refuses once; with every CPU Periodic there is no way to make `TickDevice::program_event()` fail, and binding a fake device to a running CPU's `TickDevice` would break that CPU's tick. It is now item 1 of `docs/repairs/261009/kernel-time.md` and becomes possible once a CPU runs one-shot.
- Retrying by itself. After a refusal nothing re-arms the device until the next activation, cancellation or expiry on this queue. On a one-shot CPU whose only armed timer was refused, that can be a long time. Whether the tick Timer, the lockup detector or a retry in the handler covers this is part of the one-shot work in `.agents/tasks/high-res-timers/DESIGN.md`.
- Reading the counter anywhere. `timer.queue.program_refused` is a `GKTL_COUNTER` like the dispatchers' and shows wherever those are shown.

**How it was found**
Found by the 2026-10-09 read of `kernel/time` for the high-resolution timer design (four read-only agents); see `.agents/tasks/high-res-timers/DESIGN.md`. This change's own detour: the first build failed on `Option<TimePoint<>>`, which is how the nothrow requirement above was found.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why could this bug not show in Periodic mode, even if `program_event()` had failed there?
2. Why is a cache that wrongly says "armed" worse than one that wrongly says "unknown"?
3. After a refusal, which event on this CPU's queue is the first that retries, and what would have to exist for the retry not to depend on a timer happening to be armed?

## Timer::forward() moves a timer whose deadline is exactly now (2026-10-10)

**Files**
- `kernel/kernel/time/timer.cpp` — `forward()` returns 0 only when the deadline is ahead of `now`
- `kernel/commands/timer_test.cpp` — new `timer.test` console command, five checks of `forward()`
- `kernel/commands/BUILD.gn` — lists `timer_test.cpp`
- `docs/repairs/261009/kernel-time.md` — item 4 is deleted

**Symptom**
Latent, rare, not observed. A periodic timer's handler that ran exactly on its deadline and called `forward()` got 0 back and kept the deadline. Returning `Restart` then put it back in the queue at the deadline it had just run for, and `run_expired_timers()` popped it again in the same pass, so the handler ran twice for one period. For the tick timer that is two scheduler ticks and two lockup heartbeats; for a sampler timer, a duplicate sample at one instant. Nanosecond clocks make "exactly" uncommon, which is why nothing has shown it.

**Cause**
Two places answered "is this deadline due?" and disagreed on the boundary. The queue says a timer is due when `now` is not before its deadline (`pop_expired_locked()` skips a timer only while `clock_now < deadline`), so a deadline equal to `now` is due and runs. `forward()` guarded with `expires >= now`, which read the same deadline as "not yet due" and left it alone. Each test is sensible alone; together they let a timer be both due and not due at one instant, and the gap between them is exactly the instant a handler runs at its deadline. The repair is to give the boundary one meaning, the queue's, because the queue is what decides when a handler runs at all.

**Change**
Before:

```cpp
if (expires >= now) {
    return 0;
}
```

After:

```cpp
if (expires > now) {
    return 0;
}
```

With `expires == now`, `delta` is zero, so the function skips the multi-interval branch, steps once and returns 1: the deadline is one interval ahead and exactly one deadline was passed.

**Reference**
Linux `hrtimer_forward()` (`kernel/time/hrtimer.c`): `delta = now - expires; if (delta < 0) return 0;`, so a delta of zero falls through to `hrtimer_add_expires(timer, interval)` and returns 1. This is checked against the Linux tree on this machine. No Zircon counterpart was looked for; the Linux function is the model for this one.

**Verification**
- `timer.test` on the dbg image, before the change: 4 of 5 checks passed and `exactly at the deadline: one interval` failed with `overrun 0 (want 1), deadline moved by 0 ns (want 10000000)`. After the change all 5 pass.
- `scripts/format_cpp.py --check` on `timer.cpp` and `timer_test.cpp`: clean.
- `clang-tidy -p out/bringup.x86` on both: no diagnostic in either file (25 in the headers they include, on lines not touched here).
- `ours-verify` (`boot_check.py`): dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (144 passed). The boot check does not run console commands, so `timer.test` above is what covers this change.
- The double run itself (one tick handled twice) was never reproduced in a boot: not measured.

**Deliberately not done**
- The `if (expires > now)` branch inside `delta >= interval` is unreachable: the division truncates, so `expires` after the jump is at most `now`, and the `orun++` path always follows. Linux has the same shape. It stays as it is; removing it is a separate cleanup with nothing to do with this boundary.
- A test through the real queue (a handler run at exactly its deadline). That needs the clock to land on a chosen nanosecond; `forward(now, interval)` takes `now` as an argument, so the test sets it.

**How it was found**
Found by the 2026-10-09 read of `kernel/time` for the high-resolution timer design; see `.agents/tasks/high-res-timers/DESIGN.md`. The test was written first and run on the unfixed code, which failed on the one check the repair predicted.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did `pop_expired_locked()` and `forward()` have to agree on whether a deadline equal to `now` is due, and what ran twice when they did not?
2. What does `forward(now = deadline + 2 * interval, interval)` return, and why 3 and not 2?
3. Why does `activate()` followed by `cancel()` leave a timer with a deadline and no queue, and why may `forward()` only be called on a timer in that state?
