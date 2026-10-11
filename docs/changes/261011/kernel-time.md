# kernel-time changes

## timershare.test divides each repeat by its own unloaded timing (2026-10-11)

**Files**
- `kernel/commands/timer_share_test.cpp` — the share of a repeat is its unloaded timing over its loaded one; the median and the best of the five are reported

**Symptom**
`timershare.test` failed once in the fifteen or so runs of one session: "the thread kept under 25.0 % of its CPU in 1 run(s)". That run's C = 20 µs point reported a median share of 19.6 % and, on the same line, "the work alone takes 9086 µs", where every other run said about 23 000 µs. The kernel had not changed.

**Cause**
The statistic compared two different populations. The reference was the fastest of five unloaded timings and the numerator's partner was the median of five loaded ones. Under QEMU TCG the speed of the guest drifts with the host's load and frequency, so within one point the spin loop ran at a different speed from one repeat to the next. In the failing run one unloaded repeat caught a fast spell and ran in 9 ms; the loaded repeats ran at the usual speed, and 9 ms over the usual loaded time looked like 19.6 %. A minimum is the right reference when noise can only slow a run down; it is wrong here, because the drift also speeds runs up, and the comparison needs the two timings to have had the same conditions.

The share is now taken per repeat: an unloaded timing followed at once by a loaded one, divided, five times, and the five shares are sorted. The drift between a pair is small next to the drift across the whole point, and the median of the ratios no longer depends on which repeat was lucky.

**Change**
Before, in `measure()`:

```cpp
auto const kept_median = 1000 * run->base_tsc / run->loaded_tsc[REPEATS / 2];
```

After:

```cpp
shares[rep] = 1000 * run->base_tsc[rep] / run->loaded_tsc[rep];
// ... sorted ...
auto const kept_median = shares[REPEATS / 2];
```

Ruled out: raising the 25 % floor's tolerance until the flake stopped. That would have hidden a statistic that was wrong, and a floor that is wrong in one direction is wrong in the other.

**Reference**
None, this project's own measurement.

**Verification**
- Six runs on the one-shot build after the change, QEMU 11.1.1 TCG, 4 CPUs, P = 10 µs: all pass, and the medians of the three points are 60.5 / 47.1 / 50.2, 50.1 / 51.7 / 47.6, 54.4 / 54.7 / 54.4, 61.7 / 58.5 / 46.3, 61.3 / 53.2 / 50.3 and 49.9 / 53.5 / 48.1 %. The range is 46 to 62 %, as the four runs before it gave (45 to 54 %).
- `scripts/format_cpp.py --check` and `clang-tidy -p out/bringup.x86` on the file: clean, no diagnostic in it.
- Not measured: the failing run cannot be replayed, since the host's speed at that moment is not recorded. That the cause is the statistic is shown by the 9086 µs on the failing line and by six runs of the corrected one, not by reproducing the failure.

**Deliberately not done**
- Recording the unloaded timing of every repeat in the log line. The line already reports the median of them as "the work alone takes", which is what made this failure legible.

**How it was found**
Running the test together with four others in one session and reading the failing run's line in `dbg.log`: the "work alone" figure was 2.5 times lower than in every other run.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the minimum the right reference for a timing when noise can only slow it down, and why was it wrong here?
2. Why is a ratio taken within one repeat less sensitive to the host's drift than a ratio of statistics taken across five?
3. What would a floor loosened until the flake went away have hidden?

## A tick device is swapped with interrupts off (2026-10-11)

**Files**
- `kernel/kernel/time/clock_event_device.cpp` — `enroll()` runs `check_new_device()` inside an interrupt-off region
- `kernel/kernel/time/tick_device.cpp` — `check_new_device()`, `rebind_device()` and `setup_oneshot_tick()` assert that interrupts are off; `setup_oneshot_tick()` no longer disables them itself
- `kernel/include/ours/time/tick_device.hpp` — the contract is written on both public functions
- `docs/repairs/260926/kernel-time.md` — deleted, its last item is fixed

**Symptom**
Latent undefined behaviour, never observed. The boot CPU enrols its local APIC timer at `InitLevel::Irq + 4` with interrupts on, while the HPET, which has been its tick since `Irq + 2`, is still interrupting it. `rebind_device()` then moves the HPET's handler out of the device and detaches it. If the HPET interrupt lands in the middle of that, `handle_event()` reads a handler that is half moved.

**Cause**
`ClockEventDevice::m_event_handler` is a move-only `ustl::function::Function` with its target stored inline. Moving it out is several stores, the target's bytes and then the pointers that say how to call and destroy it, and `handle_event()` is `if (m_event_handler) { m_event_handler(this); }`, a test and then a call. An interrupt on the same CPU between any two of those stores, or between the test and the call, finds the handler empty or half filled and calls through a pointer that is no longer, or not yet, the right one. It does not need another CPU: an interrupt is concurrent with the code it interrupts, and the only defence against it on the same CPU is not taking it.

It was harmless because the only handler ever installed was a captureless lambda whose target is a few trivially destructible bytes, so a call through the stale invoker still landed on valid code. That is luck about this one lambda, not a property of the move, and the one-shot entry from the first change of 2026-10-10 added a second handler to the same swap.

The APs are not exposed: they enrol with interrupts off, from `cli` in the AP entry until they become idle. That is why the boot CPU alone needed the fix, and why `setup_oneshot_tick()` had a region of its own that covered half of the swap: the new device's handler and mode, but not the old device's detach, which happens before it is called.

**Change**
Before, in `ClockEventDevice::enroll()`:

```cpp
auto const status = TickDevice::Current::get().check_new_device(&self);
```

After:

```cpp
Status status = Status::Ok;
{
    arch::AutoInterruptDisabler const no_interrupts;
    status = TickDevice::Current::get().check_new_device(&self);
}
```

The region covers `Current::get()` as well, so the CPU whose `TickDevice` is named cannot change under it. The mutex that guards the device list stays outside the region: a mutex can park the caller, and parking with interrupts off is how a CPU stops coming back. The contract is a `DEBUG_ASSERT(arch::irqs_disabled())` at the top of `check_new_device()`, `rebind_device()` and `setup_oneshot_tick()`, so a second caller of the public `rebind_device()` finds out in a debug build.

Ruled out: masking the HPET's interrupt line instead. It would leave the handler swap racing with anything else that calls `handle_event()` on that device, and the line's mask is another piece of state to restore on failure.

**Reference**
Linux `clockevents_register_device()` (`kernel/time/clockevents.c`) calls `tick_check_new_device()` under `raw_spin_lock_irqsave(&clockevents_lock, flags)`. That lock also serialises registrations against each other. Here the list is guarded by a mutex and the swap by the interrupt-off region, because the swap is per CPU and the list is global; two CPUs enrolling at once swap two different `TickDevice`s.

**Verification**
- `ours-verify` on the default build and on the `time_oneshot=true` build: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144) on the default build. The dbg image runs the three assertions on every enrolment (the PIT, the HPET and four local APIC timers) and none fired, so no caller was missed.
- With the option on every CPU still logs `tick device LVT runs one-shot`; `timer.test`, `sleep.test`, `semaphore.test` and `waitqueue.test` pass.
- `scripts/format_cpp.py --check` and `clang-tidy -p out/bringup.x86` on `tick_device.cpp` and `clock_event_device.cpp`: clean, no diagnostic in either.
- Not measured: the race itself. It needs an HPET interrupt inside a few stores, so no run would show it before or after.

**Deliberately not done**
- A test of the race. A test that makes an interrupt land in the move would be a test of the machine's timing, and a passing run says nothing about the next one. The assertions are the check that stays.
- Disabling interrupts inside `rebind_device()` itself instead of requiring it of the caller. The region has to be wider than the function, since `Current::get()` is read before it is called.

**How it was found**
By the 2026-09-26 per-CPU access audit (`docs/repairs/260926/kernel-time.md`, now deleted). It was done now because the one-shot entry made the boot CPU run this swap, with a second handler, on every boot with the option on.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does swapping a handler need interrupts off when only one CPU is involved?
2. Why is the mutex that guards the device list outside the interrupt-off region, and what would go wrong if it were inside?
3. Why were the APs never exposed, and what would change if one of them enrolled with interrupts on?
