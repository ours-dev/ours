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

## TickSched::on_tick() runs the timer queue only in Legacy mode (2026-10-10)

**Files**
- `kernel/kernel/time/tick_sched.cpp` — `on_tick()` calls `run_expired_timers()` only when the work mode is Legacy
- `docs/repairs/260926/kernel-time.md` — item 2 is deleted
- `.agents/tasks/high-res-timers/DESIGN.md` — the "What is missing" bullet for this bug is marked fixed

**Symptom**
Latent, cannot be triggered today: every CPU stays in `WorkMode::Legacy`, and nothing calls `emulate(HpTick)`. The moment a CPU enters HpTick, the tick timer's handler calls `on_tick()`, which called `run_expired_timers()` from inside `run_expired_timers()`'s own loop.

**Cause**
`run_expired_timers()` is the queue's engine, and it keeps state that is only meaningful while exactly one pass is running: it resets the armed deadline to `INFINITE_TIME` at the start and programs the device afresh at the end, and it records in `m_running` which handler is on this CPU's stack, set before the handler runs and cleared after. `m_running`'s invariant is "non-null exactly while a handler is on this CPU's stack". In HpTick the tick is a `Timer` whose handler is `TickSched::handle_event()`, so `on_tick()` runs inside a handler. A nested pass then sets `m_running` to the inner timer, clears it to null while the outer tick handler is still on the stack, and resets and reprograms the armed deadline that the outer pass will reset and reprogram again when it resumes.

The deeper mistake is that the engine had two ways in. In Legacy, `on_tick()` is right to run it, because the device is not armed for the queue's deadlines (`TickDevice::program_event()` returns Ok without writing in Periodic mode), so the tick is the only thing that ever looks at the queue. In HpTick, the device interrupt looks at it, and a second entry from the tick is both redundant and unsafe. Which entry is legitimate is a property of the mode, so the mode has to choose.

**Change**
Before:

```cpp
task::Scheduler::Current::tick(task::sched::to_sched_time(now));

TimerQueue::get().run_expired_timers();
```

After:

```cpp
task::Scheduler::Current::tick(task::sched::to_sched_time(now));

if (m_workmode == WorkMode::Legacy) {
    TimerQueue::get().run_expired_timers();
}
```

Behaviour today is unchanged: Shutdown returns before this line as before, and every CPU that gets here is Legacy. The device-interrupt entry that HpTick relies on is not written by this change.

**Reference**
Linux `hrtimer_run_queues()` (`kernel/time/hrtimer.c`) begins with `if (hrtimer_hres_active(cpu_base)) return;`: with high-resolution mode active the tick path does not touch the timer queues, and `hrtimer_interrupt()` does. Zircon has no counterpart: its tick and timer queue are one function, `TimerQueue::Tick()`, with a single entry.

**Verification**
- `scripts/format_cpp.py --check`: clean. `clang-tidy -p out/bringup.x86`: no diagnostic in `tick_sched.cpp` (148 in the headers it includes).
- `ours-verify`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (144 passed).
- The HpTick path itself was not run: nothing enters it. Not measured.

**Deliberately not done**
- The device-interrupt handler that runs the queue in HpTick, and anything that enters HpTick. Those are the one-shot work in `.agents/tasks/high-res-timers/DESIGN.md`.
- A test. HpTick cannot be entered yet, so there is nothing to run; the check belongs with the commit that enters it.

**How it was found**
By the 2026-09-26 per-CPU access audit, after the periodic-tick make-up livelock fix; the call graph was rechecked then (`run_expired_timers()` has one caller, `on_tick()`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Which two pieces of `TimerQueue` state does a nested `run_expired_timers()` corrupt, and which invariant does one of them break?
2. Why is it right for Legacy to run the queue from `on_tick()` and wrong for HpTick?
3. In HpTick, what runs the queue, and what must the device be armed for to make that work?

## kernel.time.oneshot: a boot option, off by default, and the build argument that sets it (2026-10-10)

**Files**
- `kernel/include/ours/boot/options.hpp` — `TIME_ONESHOT_OPTION`, the `TimeOneshot` flag and `BootOptions::time_oneshot()`
- `kernel/top/boot_options.cpp` — `apply()` reads the option as a boolean flag
- `kernel/kernel_params.gni` — build argument `time_oneshot`, false
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` — with `time_oneshot` true the image's command line gains `kernel.time.oneshot=true`

**Symptom**
None: nothing reads the option yet. It exists so that the one-shot work in `.agents/tasks/high-res-timers/DESIGN.md` can be switched on for one boot and off for the next with the same kernel, and so that both settings can be verified.

**Cause**
The one-shot path changes what every CPU's tick device does from the moment it comes up, and its first version cannot be tested on real hardware (TSC-deadline is not exercised under QEMU TCG). A boot option rather than a compile-time switch keeps one kernel binary for both settings, so a failure with it on can be compared with the same build with it off. The option is read where the CPU's timer device comes up, which is `InitLevel::Irq + 4` on the boot CPU, after `setup_options()` has run.

The command-line plumbing follows the existing options: a flag in `BootOptions::Flags`, written only by `apply()` on the boot CPU before anything reads it, and read-only after. All bits of the word are written by the one function in the one place, so they share a protection.

**Change**
The option is a bare key or `kernel.time.oneshot=<bool>`, parsed like `kernel.shell` and `kernel.panic.reboot` (`apply_flag()`). The build argument `time_oneshot` decides only what the dev images' command lines say; it does not change the compiled kernel. Both dev images take it, because the kernel shell that runs `timer.test` and `lockup status` is on dev.dbg_1 and the rel image is the one with the user-space shell.

Ruled out: a compile-time `constexpr` switch like `SCHED_NOHZ`. It would need two kernels to compare, and the point of the switch is to compare one kernel booted two ways.

**Reference**
None, this project's own design. Linux selects the same thing at run time with the `highres=` kernel parameter (`hrtimer_is_hres_enabled()`, `kernel/time/hrtimer.c`); Zircon has no switch.

**Verification**
- `scripts/format_cpp.py --check` and `scripts/format_gn.py --check` on all five files: clean.
- A second build directory with `tools/set.py bringup.x86 --out out/oneshot.x86 --args time_oneshot=true`: `kernel.dev.obi` and `kernel.dev.dbg_1.obi` both contain `kernel.time.oneshot=true` (`strings` finds it once in each); the default build's images do not.
- `ours-verify` on the default build: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (144 passed). On the `time_oneshot=true` build (host tests skipped): dbg PASS and rel PASS with the same counts, as expected since nothing reads the option.
- That the option is parsed to `time_oneshot() == true` is not shown by this change alone; the next change reads it and logs it at boot.

**Deliberately not done**
- Reading the option. That is the next change.
- A host test of the parsing. `boot::BootOptions` lives in the kernel and is not built for the host; the option shares `apply_flag()` with the three flags that already work.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the switch a boot option and not a `constexpr` like `SCHED_NOHZ`?
2. At which init level does the boot CPU first need the option, and why is it already parsed by then?
3. Which images does `time_oneshot` change, and which of them can run `timer.test`?

## With kernel.time.oneshot on, a CPU's timer queue arms its own device and the tick is a timer in that queue (2026-10-10)

**Files**
- `kernel/kernel/time/tick_device.cpp`, `kernel/include/ours/time/tick_device.hpp` — a per-CPU timer device is put into Oneshot mode when it is enrolled (`wants_oneshot_tick()`, `setup_oneshot_tick()`), its handler is the new `handle_oneshot_event()`, and `switch_to_oneshot()` is removed
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp` — `rearm_device()` arms the device from scratch at the moment it changes mode; `num_refused_programs()` reads the refusal counter
- `kernel/kernel/time/tick_sched.cpp`, `kernel/include/ours/time/tick_sched.hpp` — the tick timer's handler advances jiffies; the start hook is renamed `start_ticks()` and puts the boot CPU in HpTick when its device is one-shot; `is_shutdown()`
- `kernel/arch/x86/apic/local_apic.cpp` — the local APIC timer gains `stop_oneshot()`
- `kernel/kernel/task/sched/sched_clock.cpp` — `reset_preempt()` asserts the device is not one-shot
- `kernel/kernel/task/dpc.cpp` — one comment names the renamed hook
- `kernel/commands/timer_test.cpp` — `timer.test` measures how late 1 ms timers fire
- `docs/repairs/261009/kernel-time.md`, `.agents/tasks/high-res-timers/DESIGN.md` — item 3 is deleted; two bullets and the decision on the guard are updated
- `docs/repairs/261010/kernel-time.md` — two items found on the way
- `.agents/tasks/high-res-timers/STATUS.md` — new, where the task stands
- `.agents/skills/ours-kernel-design/references/task.md` — the Time section records how a CPU enters one-shot

**Symptom**
Every software timer fired at the next tick. HZ is 100, so a 1 ms timer was on average 5 ms late and up to 10 ms; the sampler's 10 µs period sampled every 10 ms, which is why its stress test checked only a few dozen records per boot. The cause was not a bug in any one line but that the pieces for one-shot timers had never been joined (`.agents/tasks/high-res-timers/DESIGN.md`).

**Cause**
Three facts held together to keep timers tick-granular. `TickDevice::program_event()` returned Ok without writing in Periodic mode, so the queue's earliest deadline never reached the hardware. `run_expired_timers()` had exactly one caller, `TickSched::on_tick()`, so nothing else ever looked at the queue. And the one function that would have moved a CPU to one-shot, `switch_to_oneshot()`, had no caller and set the mode before it knew the switch worked (repair item 3 of 261009, now gone with it).

The structure had one fact kept twice and one device with three writers: the mode lived in `TickDevice` and again in `TickSched`, and the device was written by the periodic re-arm, by `TimerQueue` and by `SchedClock`. Wiring a switch into that structure would have kept all three. So the change does not switch a running tick. It gives the CPU a different shape from the moment its own device exists.

**Change**
Objects, per CPU: the local APIC timer (a `ClockEventDevice`), the `TickDevice` that binds it, the `TimerQueue`, and `TickSched` with its tick `Timer`. Events with `kernel.time.oneshot` on:
1. The boot CPU's early ticks come from the global HPET, Periodic, as before; the local APIC calibrates against them.
2. The local APIC timer is enrolled on a CPU. `rebind_device()` sees the option on and a device that is that CPU's own and can be armed for one instant, so it calls `setup_oneshot_tick()` instead of `setup_periodic()`.
3. With interrupts off, `setup_oneshot_tick()` installs `handle_oneshot_event()` as the device handler, switches the device to Oneshot and only then records `m_mode = Oneshot`, so a refusal leaves the CPU Periodic and the caller falls back.
4. `TimerQueue::rearm_device()` forgets what the device was thought to be armed for and arms it for the queue's earliest deadline, or parks it when the queue is empty. Until now programming was a no-op, so the queue's record named a deadline that had never been written.
5. If the CPU's scheduler exists (every CPU but the boot CPU), `emulate(HpTick)` arms the tick timer. The boot CPU has no scheduler yet at `InitLevel::Irq + 4`, so `start_ticks()`, at `InitLevel::Threading`, enters HpTick on it.
6. From then on the device interrupt runs `run_expired_timers()`; the tick timer's handler calls `on_tick()` and advances jiffies by what `forward()` says went by, on the one CPU that owns them.

Two drivers' details this needed. The local APIC `set_mode_oneshot()` arms a whole tick period as it switches on, so a CPU with an empty queue had to be able to take that back: `stop_oneshot()` writes zero to the initial count (counting mode) or to the TSC-deadline MSR. And the `OneshotStopped` state the tick layer already used for "parked" was unsupported by this driver, which would have counted a refusal every time a queue emptied.

Before and after for the one function the old design could not do (`kernel/kernel/time/tick_device.cpp`):

```cpp
// before
auto TickDevice::switch_to_oneshot(ClockEventDevice::Handler handler) -> Status {
    m_mode = Mode::Oneshot;
    return setup_oneshot(m_clkdev, ustl::move(handler), SteadyClock::now());
}
```

```cpp
// after: the mode is committed after the device accepted it
dev->set_event_handler([](ClockEventDevice *) { handle_oneshot_event(); });
auto const status = dev->switch_mode(ClockEventDevice::Mode::Oneshot);
if (Status::Ok != status) {
    return status;
}
m_mode = Mode::Oneshot;
```

Ruled out:
- Entering one-shot from the running periodic tick, as Linux does: it is the switch of a live tick this change exists to avoid, and needs a caller on each CPU to notice the right moment.
- Entering HpTick on the boot CPU at device enrolment: `on_tick()` would call into a scheduler that `task::init()` has not built. The cost is that the boot CPU has no tick between `Irq + 4` and `Threading`; nothing reads one then, and it is repair item 2 of `docs/repairs/261010/kernel-time.md`.
- A compile-time switch: see the option's entry above.

**Reference**
Linux enters one-shot from the running tick: `tick_check_oneshot_change()` finds a one-shot-capable device, `hrtimer_switch_to_hres()` and `tick_switch_to_oneshot()` (`kernel/time/tick-oneshot.c`) swap the handler for `hrtimer_interrupt()` and `tick_setup_sched_timer()` turns the tick into an hrtimer. The shape here is Linux's, with the tick a timer in the one queue; the entry is this project's own (R1 in the DESIGN), because the queue is Zircon's. Zircon: `TimerQueue::Tick()` is the device interrupt's handler and `UpdatePlatformTimerLocked()` arms the device for the earliest deadline (`zircon/kernel/kernel/timer.cc`); it has no tick to keep, which is why the tick timer is the part with no counterpart there.

**Verification**
Two builds of the same commit: `out/bringup.x86` (option off) and `out/oneshot.x86` (`tools/set.py bringup.x86 --out out/oneshot.x86 --args time_oneshot=true`). QEMU 11.1.1 TCG, 4 CPUs.

| | option off | option on |
|---|---|---|
| `timer.test`: 1 ms timers, lateness range (24 samples) | 95 to 10082 µs | 107 to 2743 µs |
| `timer.test`: median lateness | 5046 µs (4891 µs on another run) | 988 µs (five more runs: 802, 1022, 955, 1011, 925 µs) |
| `lockup status`: ticks per second per CPU (cpu0, over 0.84 s) | 100.0 | 99.3 |
| sampler stress test, records checked, dbg | 46 | 1278 |
| sampler stress test, records checked, rel | 52 | 1465 |
| `profile` example, kernel samples, rel | 40 | 216 (49 dropped: the 64-record per-CPU buffer filled at its 1 ms period) |

- `ours-verify`, both builds: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144) on the default build.
- With the option on, every CPU logs `tick device LVT runs one-shot`, and `timer.test` asserts that the median lateness is under 2 ms. With the option off it logs the median and does not assert it. Both ways it asserts that every timer fired, none before its deadline, and that no deadline was refused since boot.
- `sleep.test`, `semaphore.test`, `waitqueue.test` and `mutex.stress 2000` pass on both builds. `mutex.stress` with its default 20000 rounds is called STUCK on both builds and on the untouched base commit 8ab9400b: the known slow run in `docs/repairs/261003/kernel-commands.md` item 1.
- `scripts/format_cpp.py --check` on every touched C++ file: clean. `clang-tidy -p out/bringup.x86` on `tick_device.cpp`, `tick_sched.cpp`, `timer_queue.cpp`, `timer_test.cpp`, `local_apic.cpp`, `sched_clock.cpp`, `dpc.cpp`: no diagnostic in any of them.
- Not measured: the TSC-deadline paths (`stop_oneshot()` and the arming in deadline mode), because TCG does not provide them; real hardware; a lateness figure other than at 1 ms; an AP's first tick against the scheduler's idle thread other than by the boots above.

**Deliberately not done**
- The guard against a callback slower than its period, which in one-shot mode fires back to back. It is the next step. Until it lands the option stays off by default and the sampler is not run on it with a period below what its callback costs.
- Moving the preemption deadline into the queue (repair item 2 of 261009). `reset_preempt()` only asserts now, which makes the double writer fail loudly instead of silently if `sched_nohz` is turned on with the option.
- Deleting the second copy of the mode (`WorkMode::Legacy` against `HpTick`). It stays, because the option is off by default and both paths run. `HpTick` is entered only through `emulate()` as before.
- Marking a clock source `HpClock` (repair item 2 of 260903). `select_best_source()` reads the mode only when a source is enrolled, and the HPET is enrolled at `Irq + 2`, before any CPU is one-shot, so nothing needs it yet.
- Wrapping the old device's detach in `rebind_device()` in an interrupt-off region (260926 item 1). The new path's swap is inside one; the HPET's detach on the boot CPU still is not.

**How it was found**
The two things that were not in the design came from reading the driver rather than from a failing boot: `ApicTimerEvent` has no `stop_oneshot` (its vtable slot is filled by name, so the absence is silent until `switch_mode(OneshotStopped)` returns Unsupported), and `set_mode_oneshot()` writes an initial count of one period. The boot CPU's scheduler being absent at device enrolment came from asking what `on_tick()` does in the Shutdown work mode.

**The user's prediction**
Not given. The DESIGN's expectation was that logged elapsed times drop from about 10 ms to near the request. Measured: median lateness of a 1 ms timer from about 5 ms to about 0.9 ms, with a tail of up to about 3 ms that is the host's scheduling under TCG.

**Three questions you should be able to answer**
1. Why does the boot CPU's tick timer start from a hook at `InitLevel::Threading` while every other CPU's starts when its device is enrolled?
2. Why must `setup_oneshot_tick()` call `rearm_device()`: what would the queue believe about the device if it did not, and what would a timer armed before the mode changed do?
3. What in the tree still writes a one-shot device behind the queue's back, and what stops that from happening today?

## timershare.test measures how much CPU a thread keeps under a periodic timer slower than its period (2026-10-10)

**Files**
- `kernel/commands/timer_share_test.cpp` — new console command `timershare.test`
- `kernel/commands/BUILD.gn` — lists it
- `docs/architecture/timer/callback-cpu-share.md` — new note: the formula for the share, and what was measured against it
- `docs/repairs/261010/kernel-time.md` — items 4 and 5
- `.agents/tasks/high-res-timers/DESIGN.md`, `.agents/tasks/high-res-timers/STATUS.md` — where the guard stands

**Symptom**
Not a defect. The second step of `.agents/tasks/high-res-timers/DESIGN.md` is a guard for a periodic timer whose callback is slower than its period, which in one-shot mode could interrupt a CPU back to back and leave the thread it interrupts little or nothing. Whether that happens, and how bad it is, had not been measured.

**Cause**
The case needs a measurement before a design. A callback of cost C that ends with `forward(P)` leaves the thread the time g = P − (C mod P) before the next interrupt, so a share of s = g / (C + g) of its CPU (the note under `docs/architecture/timer/`). That treats the interrupt's entry, exit and re-arming as free and assumes the device delivers within g. Neither was known to hold here.

**Change**
`timershare.test` pins one thread to the last CPU with preemption off, so that only an interrupt can take the CPU from it. The thread does a fixed amount of work, sized to about 25 ms alone, and times it with the TSC: first with nothing else on that CPU, then with a timer of period 10 µs armed on it whose callback burns C on the TSC and ends with `forward(P)`, as the tick and the sampler do. The ratio of the two times is the share. Each point is repeated five times; the reference is the fastest unloaded timing and the loaded side is reported at its fastest and at its median. It also reports the C the callback really took, the spacing between callbacks, and what one read of the clock source costs. It passes unless the thread's median share is under 25 % (no starvation); the formula's figure is printed beside the measured one and not asserted.

Three things went wrong on the way, each worth keeping in mind for the next measurement:
- The loop first read the clock source every 256 spins. On this setup a read costs about 8 µs and contends with the other CPUs' log timestamps, so the loop measured the contention: the share came out above 900 % in one run. Timing a fixed amount of work by the TSC removed it.
- The scheduler gave the CPU to the dbg image's logging thread for a whole slice between two timings. Preemption is now off for the whole measurement.
- `scripts/kshell_run.py` waits for the shell's prompt after a command, unless the command's name ends in `.test`, when it waits for the verdict line. The prompt is broken up by the logging threads' lines, so `mutex.stress 4500` and a first `timer.share` both looked as if they had hung. The command is `timershare.test` for that reason.

**Reference**
None, this project's own measurement. Linux `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) bounds the same thing from the other side: when the next timer is already due after the callbacks it retries up to three times, and if it is still due it marks a hang and arms the device `min(100 ms, the longest hang)` ahead, so that the code interrupted gets at least as long as the interrupt took.

**Verification**
Four runs on the one-shot build and two on the periodic one, QEMU 11.1.1 TCG, 4 CPUs, P = 10 µs:

| C asked | callback took | thread's share at the median, one-shot | callbacks spaced, one-shot | thread's share at the median, periodic |
|---|---|---|---|---|
| 20 µs | 61 to 66 µs | 48 to 54 % | 360 to 398 µs | 88 to 105 % |
| 45 µs | 90 to 94 µs | 50 to 51 % | 384 to 418 µs | 82 to 89 % |
| 100 µs | 149 to 151 µs | 45 to 52 % | 481 to 511 µs | 90 to 92 % |

The formula, with the C the callback really took, gives 1 to 10 %. The measured share is much higher, and the same for every C. Taking the spacing, the callback and the share together, each cycle is the callback, about 110 µs of interrupt work outside it (derived, not timed separately), and about 200 µs in which the thread runs, whatever C is. A clock read costs 7 to 10 µs, so the callback's own `forward()` and the queue's reads account for part of the 110 µs.
- Periodic mode behaves as expected (one callback per tick, about 10 ms apart, the thread keeps what is left); its spread of about ±10 points is the noise of a 25 ms window on a TCG host.
- `scripts/format_cpp.py --check`, `format_gn.py --check` and `clang-tidy -p out/bringup.x86` on `timer_share_test.cpp`: clean, no diagnostic in the file.
- Not measured: anything where an interrupt is cheap to deliver (real hardware, TSC-deadline, KVM); the formula's regime, in which the next interrupt comes within g, has not been seen.

**Deliberately not done**
- The guard. On this platform a 10 µs timer with a callback over ten times its period leaves the thread about half the CPU, because the emulated timer cannot deliver sooner than about 200 µs after it is armed. There is no starvation to guard against and nothing to check a guard with. Whether real hardware needs one is `docs/repairs/261010/kernel-time.md` item 5.
- Tuning the 110 µs of per-interrupt work. That is performance work and goes through the user first: item 4 of the same file.

**How it was found**
Not a bug. The three wrong turns above are the debugging content: a share above 900 % said the baseline was not a baseline; a callback that "took" 66 µs for 20 µs asked said the clock read inside `forward()` was part of the cost; a prompt that never came back, reproduced by a command that had nothing to do with timers, said the harness was at fault.

**The user's prediction**
Not given. The expectation I put to the user was that the measured share would be lower than the formula's, from overheads the formula leaves out. It is higher, by a factor of five or more, because the device cannot deliver as fast as the formula assumes.

**Three questions you should be able to answer**
1. Why does the formula give 1 to 10 % while the thread keeps about half its CPU, and which of the formula's assumptions does this platform break?
2. Why is the thread's CPU measured as the time for a fixed amount of work and not as the work done in a fixed time?
3. Why can this platform not tell us whether a guard is needed on real hardware?
