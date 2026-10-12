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

## TickSched keeps whether the scheduler exists and nothing else; how the tick is produced is the tick device's mode (2026-10-11)

**Files**
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp` — `WorkMode`, `emulate()` and its three accessors are removed; `start()`, `is_started()` and the private `arm_timer()` take their place; the start hook is a plain loop
- `kernel/kernel/time/tick_device.cpp` — `setup_oneshot_tick()` arms the tick timer unconditionally
- `docs/repairs/261010/kernel-time.md` — item 2 is deleted (the other items keep their numbers, which other records cite)
- `.agents/tasks/high-res-timers/DESIGN.md`, `.agents/tasks/high-res-timers/STATUS.md` — updated

**Symptom**
No failure. Item 2 of `docs/repairs/261010/kernel-time.md` said that with `kernel.time.oneshot` on, the boot CPU has no tick between its local APIC timer being enrolled (`InitLevel::Irq + 4`) and the start hook (`InitLevel::Threading`), so jiffies skip that window and the lockup heartbeat does not beat in it. Measured now: a temporary log line in the tick timer's handler, printing when it runs before the scheduler has started, printed nothing in a boot, with the probe confirmed to be in the binary. The window is shorter than one tick (10 ms), so a tick is lost at most once and in this boot never. The item overstated the symptom.

**Cause**
The real fault is the one R1 of the DESIGN names. `TickSched` kept `WorkMode`, whose Legacy and HpTick are a second copy of what `TickDevice`'s mode already says, Periodic and Oneshot; and the same field's third value, Shutdown, meant "the scheduler does not exist yet". Two facts in one field forced an order on things that have none. The tick timer could only be armed in HpTick, HpTick could only be entered once the scheduler existed, so the boot CPU's tick had to wait for the start hook; and the hook then had to ask whether this CPU's device was one-shot, set every CPU Legacy, and set the boot CPU HpTick, with each other CPU flipping itself later. And `on_tick()` chose whether to run the queue from `WorkMode`, a copy that could disagree with the device: a CPU marked HpTick whose device was still Periodic would have run no timer at all.

Now each fact has one owner. `TickDevice::mode()` says how the tick is produced, and `TickSched::m_started` says whether the scheduler exists. They are independent, so the tick timer starts when the device does, the hook only marks every CPU's scheduler started, and `on_tick()` reads the device for the one decision that depends on it.

**Change**
Before, in `TickSched::on_tick()`:

```cpp
if (m_workmode == WorkMode::Legacy) {
    TimerQueue::get().run_expired_timers();
}
```

After:

```cpp
if (TickDevice::Current::is_mode_periodic()) {
    TimerQueue::get().run_expired_timers();
}
```

and the start hook went from setting every CPU Legacy and then the boot CPU HpTick if its device was one-shot, to:

```cpp
cpu_possible_mask().for_each([](CpuNum cpu) { TickSched::get(cpu).start(); });
```

`m_started` is a `std::atomic<bool>` with a release store in `start()` and an acquire load in `is_started()`: the store publishes the scheduler that `task::init()` built, and the load is what makes it safe to hand a tick to it. It is written for another CPU by the boot CPU, which is why a plain `bool` would not do for the APs.

Ruled out: keeping `WorkMode` and adding a boolean beside it. That makes three states to keep in agreement instead of two.

**Reference**
Linux keeps the device's mode in `tick_device::mode` and records NOHZ and HIGHRES again in `tick_sched::flags` (`TS_FLAG_NOHZ`, `TS_FLAG_HIGHRES`, `kernel/time/tick-sched.h`); this change goes the other way and keeps the one. Zircon has no counterpart: its tick and timer queue are one function, `TimerQueue::Tick()`.

**Verification**
- `ours-verify` on the default build and on the `time_oneshot=true` build: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144) on the default build.
- On both builds `lockup status` shows every CPU ticking, and `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test` and `waitqueue.test` pass.
- `scripts/format_cpp.py --check` and `clang-tidy -p out/bringup.x86` on `tick_sched.cpp` and `tick_device.cpp`: clean.
- Not shown by this change: any gain. The `lockup status` figures cannot show the window, because the boot CPU's tick count includes the HPET ticks from before its local APIC was enrolled (cpu0 is about 25 ticks ahead of the others both before and after).

**Deliberately not done**
- `m_last_tick` and `m_next_tick`, which nothing reads: they are scaffolding for the tick-stopping work and stay.
- The Periodic path. It is still what a CPU runs until its own device is enrolled, and the whole of the tree when the option is off; removing it is the decision to make the option the default, which is the user's.

**How it was found**
By following item 2 of the repairs to its cause. The probe was added and removed in one session (`git diff` shows no trace of it) to find out what the item cost; it cost nothing measurable.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `on_tick()` run the queue when the device is Periodic and not when it is Oneshot, and where is that decided now?
2. Why is `m_started` an atomic with release and acquire although each CPU only reads its own?
3. What did the repair item claim that the probe disproved, and what was the real reason to make the change?

## A global timer device serves several CPUs through a broadcast group, and a CPU can move its tick to another device (2026-10-11)

**Files**
- `kernel/include/ours/time/tick_broadcast.hpp`, `kernel/kernel/time/tick_broadcast.cpp` — new: `TickBroadcast`, one global device armed for the earliest deadline of the CPUs that rely on it
- `kernel/kernel/time/tick_device.cpp`, `kernel/include/ours/time/tick_device.hpp` — `replace_with()` moves a Oneshot CPU's tick to another device, its own or a group's; `program_oneshot()` is the step shared with the group
- `kernel/kernel/time/clock_event_device.cpp`, `kernel/include/ours/time/clock_event_device.hpp` — `mark_failed()` / `is_failed()` and `for_each_enrolled()`
- `kernel/include/ours/ipi_event.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp`, `kernel/arch/x86/kernel/mp.cpp`, `kernel/arch/x86/kernel/faults.cpp`, `kernel/arch/x86/kernel/include/ours/arch/x86/faults.hpp` — `IpiEvent::Timer`, the vector `IrqVec::IpiTimer` and its handler
- `kernel/arch/x86/kernel/time/hpet.cpp` — the HPET gains `stop_oneshot()`
- `kernel/commands/tick_broadcast_test.cpp`, `kernel/commands/BUILD.gn`, `kernel/kernel/time/BUILD.gn` — `tickbcast.test` and the new sources
- `docs/repairs/260903/kernel-time.md`, `.agents/tasks/high-res-timers/DESIGN.md`, `.agents/tasks/high-res-timers/STATUS.md` — updated

**Symptom**
Not a defect; a capability that was missing. A CPU whose own timer device cannot be used had no other source of ticks: `TickDevice::check_new_device()` leaves a device that loses the selection aside, "broadcast is the piece not built yet". The two situations it is for are a device that fails and, on real machines, a local APIC timer that stops in a deep idle state.

**Cause**
A global device (the HPET) interrupts one CPU, whichever its line is routed to. It cannot be another CPU's tick device the way a local APIC timer is: the interrupt arrives on the boot CPU, `TimerQueue::get()` there is the boot CPU's queue, and the CPU that wanted the deadline hears nothing. `irq::set_affinity()` is a stub, and one global line could not follow two CPUs in any case.

So the global device is shared. A `TickBroadcast` is the group of CPUs that rely on one such device. A member's queue does not program the device; it tells the group its earliest deadline, and the group arms the device for the earliest of all. When the device fires, on whichever CPU it reaches, the handler takes the members whose deadline has come out of its books, re-arms the device for the rest, and then lets the due members run their queues: itself directly if it is one, the others by a timer IPI. A member's queue ends by telling the group its next deadline, which puts it back. The re-arming comes before the queues run because a queue can take long, and the other members' deadlines should not wait behind it.

Two details needed fixing before it could work. The HPET had no `stop_oneshot()`, so a group whose members all had nothing due would have been told "unsupported" on every parking, as the local APIC timer was before its own `stop_oneshot()`. And the IPI: `IpiService::sync_exec()` runs a function on other CPUs and waits for them, which needs interrupts on in the caller, and a device handler runs with them off; a new event, `IpiEvent::Timer`, with a vector of its own, is sent and forgotten.

**Change**
The API, in the order a CPU uses it:

```cpp
auto *group = TickBroadcast::of(device);             // thread context: takes a lock
// interrupts off, on the CPU that moves:
tick.replace_with(device, group);                    // or (own_device, nullptr) to go back
```

`replace_with()` lets go of the device in service (its handler taken out and the device detached, or the CPU leaves its group), puts the new one into service in Oneshot mode, and calls `TimerQueue::rearm_device()`, because the queue's record of what the device is armed for belongs to the device just left. A CPU's `TickDevice::program_event()` goes to `m_group->set_deadline()` when it has a group. On failure the CPU is put back on the device it left, if that works.

Lock order: a member's queue lock, then the group's, since `set_deadline()` is called from `reprogram_locked()`. The group's handler drops its lock before it runs a queue or sends an IPI, and the queue never takes the group's lock to run, so neither order can invert.

Ruled out:
- Routing the device's interrupt to the CPU that needs it. The affinity call is a stub, and the line serves one CPU at a time.
- `IpiService::sync_exec()` for the notification, for the reason above.
- Sending the notification through the member device's own `broadcast()` slot, which the local APIC timer fills with an IPI on its timer vector. A vector of its own says what it means and does not pretend to be the timer interrupt.

**Reference**
Linux `kernel/time/tick-broadcast.c`: `tick_broadcast_oneshot_mask` is the member set, `tick_broadcast_set_event()` arms the broadcast device for the earliest, `tick_handle_oneshot_broadcast()` is the device's handler, which collects the CPUs whose expiry has come and wakes them with `tick_do_broadcast()` through the member device's `broadcast` method. The shape is the same; the difference is the notification, above. Zircon has no counterpart: every CPU has a timer of its own.

**Verification**
- `tickbcast.test`, on the one-shot build: the last two CPUs are moved to the HPET together, each arms twenty timers of 1 ms one after another, all fire, then each goes back to its local APIC timer and leaves the group, the group has sent IPIs, and no device has refused a deadline: 13 checks, five runs in a row all pass. The median lateness of the timers in the group was 289 to 348 µs. On the default build it says the option is off and passes.
- `ours-verify` on both builds: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144). `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test`, `waitqueue.test` pass with the option on.
- `scripts/format_cpp.py --check` on every touched file, `format_gn.py --check` on both GN files, `clang-tidy -p out/bringup.x86` on the sources: clean; the two diagnostics in `hpet.cpp` are the old `id_` and `regs_mmio_` member names, not touched here.
- An observation I cannot explain yet: the 1 ms timers on the HPET, through an IPI, are late by about 300 µs at the median, where the same timers on the CPUs' own local APIC timers were late by about 900 µs. The emulated local APIC timer is probably the slower of the two; no measurement says so.
- Not measured: anything on hardware, a CPU that is idle in a deep state, and more than two members.

**Deliberately not done**
- Moving a CPU automatically when its device refuses. This change gives the move; the trigger, and the choice of the replacement, are the next one (`docs/repairs/261010/kernel-time.md` item 1).
- The PIT as a second group device. After `hpet_init()` the HPET holds the one legacy timer line (IRQ0), and the PIT's interrupt no longer reaches the CPU, so only the HPET can be a group device on this machine.
- The group device itself failing. A refusal by it is counted and the members are left as they are; replacing the device under every member is the same work as the automatic replacement above, applied to a group.
- `ClockEventDevice::broadcast()` and its slot, which stay as they were.

**How it was found**
Not a defect. The HPET's missing `stop_oneshot()` was found by reading what the group does when its members all have nothing due; the test did not need it, because the tick timer keeps every member's deadline finite.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can the HPET not simply be a tick device for the second CPU, and what does the group do about it?
2. Why does the group's handler take the due members out and re-arm the device before it runs any queue?
3. Why is the notification a new IPI and not `sync_exec()`?

## A CPU whose tick device refuses a deadline moves its tick to another device by itself (2026-10-11)

**Files**
- `kernel/kernel/time/tick_device.cpp`, `kernel/include/ours/time/tick_device.hpp` — `device_refused()` and `pick_replacement()`; `rebind_device()` hands a Oneshot CPU's swap to `replace_with()`, and `setup_oneshot()` is removed
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer.cpp` — a refusal raises `m_refused`, and `notify_refusal()` hands it to the tick device after the lock is dropped, from the four places that reprogram
- `kernel/kernel/time/clock_event_device.cpp`, `kernel/include/ours/time/clock_event_device.hpp` — a table of the enrolled devices that needs no lock; `clear_failed()`; the retry's log line is a warning
- `kernel/commands/tick_replace_test.cpp` — new: `tickreplace.test`; `kernel/commands/BUILD.gn`
- `kernel/commands/timer_test.cpp`, `kernel/commands/tick_broadcast_test.cpp` — their "no device refused" checks compare counters before and after
- `docs/repairs/261010/kernel-time.md`, `docs/repairs/261009/kernel-time.md`, `.agents/tasks/high-res-timers/DESIGN.md`, `.agents/tasks/high-res-timers/STATUS.md`, `.agents/skills/ours-kernel-design/references/task.md` — updated

**Symptom**
Latent, as the repair item it closes said (`docs/repairs/261010/kernel-time.md`, item 1 of that file): if the device refuses the deadline at the end of an interrupt, a one-shot CPU has nothing armed, and since its tick is a timer in the same queue, it stops ticking until the lockup detector reports it. No device in the tree refuses today, so nothing showed it; `tickreplace.test` produces it on purpose.

**Cause**
The previous changes of 2026-10-10 and 2026-10-11 gave the pieces: a refusal is recorded instead of hidden, a device can be marked failed, a CPU can move its tick to another device or to a group. What was missing was the decision, and a place where it could be made. The queue learns of a refusal inside `reprogram_locked()`, under its lock; the tick device that has to act takes locks of its own, and a swap is not for a handler to make half way through the code that found the problem.

Two designs, one of which did not work. The first handed the swap to a deferred call (a Dpc) on that CPU, because choosing a device then took a mutex. It passed no test and failed the first: the replacement was asked for, and ten seconds later the lockup detector reported CPU 3 as having taken no tick and panicked. The Dpc's worker thread never ran. The CPU had just lost its only timer device, so nothing could preempt the thread it was running (another that does nothing but log), and a thread that never blocks never gives the worker its turn. Waking the worker was not enough either: a wake with interrupts off only records that a reschedule is owed, and it is paid when preemption is next enabled, which a spinning thread never does. A recovery that needs the scheduler fails exactly when the scheduler has nothing to preempt with.

So the decision is made in the context that found out, and nothing in it takes a mutex. The devices to choose among are read from a table that only grows and needs no lock: each device is appended to it, under the mutex, as it enrols, and `for_each_enrolled()` reads it with an acquire load of the count. `TimerQueue::notify_refusal()` runs after the lock is dropped, with interrupts off, and calls `TickDevice::device_refused()`.

**Change**
Objects and events, for one CPU:
1. The queue arms the device and is refused; it counts it, forgets what the device holds, and raises `m_refused`.
2. Whoever reprogrammed (activation, cancellation, the end of an expiry pass, a re-arm) drops the lock and calls `notify_refusal()`. It returns at once if nothing was refused; otherwise it turns interrupts off, checks it is still on the queue's own CPU (a caller that migrated leaves the flag for the next visit), clears the flag and calls `device_refused()`.
3. `device_refused()` marks the device in service failed and loops: `pick_replacement()` names the best device that has not failed, can be armed for one instant and can serve this CPU, its own before a global one, then by rating, and the only global one that counts is `global_clock_event()`, the device whose interrupt the legacy timer line delivers (the PIT would be put in service and never heard once the HPET holds IRQ0). `replace_with()` moves the tick and arms the queue on it.
4. If the new device refuses as well, that comes back through step 2 from inside `replace_with()`, marks it failed and picks the next. The loop ends when the device in service has not failed, within as many rounds as there are devices. If none is left the CPU has no tick, and `device_refused()` says so as an error.

`rebind_device()` had its own way of swapping a device on a Oneshot CPU, carrying the old handler and deadline over, and it never told the queue: the queue's record of what the device was armed for named a deadline the new device had not been given. It now calls `replace_with()`, which does tell it, and `setup_oneshot()` is gone.

The policy is blunt on purpose: a refusal that reaches the queue has already been retried ten times at growing deltas by the device layer, so it retires the device for good (`clear_failed()` is the way back, for a test or for someone who knows better). A transient refusal would cost the device too, and the HPET's "too close" answer is the one that can be transient; the retry loop is what keeps that from reaching here.

Ruled out: a thread to do it, above. Taking the device list's mutex in `device_refused()`, which can park a caller that has interrupts off.

**Reference**
Linux replaces a tick device when one is unbound or a better one enrols, not when programming it fails (`tick_install_replacement()`, `tick_check_new_device()` in `kernel/time/tick-common.c`, `clockevents_exchange_device()` and `clockevents_replace()` in `kernel/time/clockevents.c`); it avoids the failure by programming with `force=1`, which clamps a close deadline to the minimum delta. Here the device layer does the same (`ClockEventDevice::program_event()` with `LateDeadline::Force`), and a refusal that survives it is treated as the device being gone. Zircon has no counterpart.

**Verification**
- `tickreplace.test`, on the one-shot build: a device rated 1000 that refuses everything is given to the last CPU. The CPU ends back on its own local APIC timer, the refusing device is marked failed, ten timers of 1 ms all fire; then with the CPU's own device marked failed too, a second refusing device sends the CPU to the HPET through a group, ten timers fire there, and the test puts it back and clears the mark. Nine checks. Run 4 times in a row inside the full sequence `tickreplace.test`, `tickbcast.test`, `timer.test`, `timershare.test`, `sleep.test`, all pass; the default build skips it.
- The first version, with the Dpc, failed that test with the lockup detector's panic; recorded above.
- `ours-verify` on both builds: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144). `semaphore.test`, `waitqueue.test`, `mutex.stress 2000` pass on both builds.
- `scripts/format_cpp.py`, `clang-tidy -p out/bringup.x86` on every touched source: no diagnostic in any of them.
- Not measured: a real device that refuses, hardware, and the migration case in `notify_refusal()`.

**Deliberately not done**
- A CPU with no device left falls back to nothing. It could go back to a Periodic one, which would give its timers tick granularity and keep it alive; that needs the tick timer cancelled on leaving Oneshot. Recorded as `docs/repairs/261010/kernel-time.md` item 6.
- The group's own refusal. When the group's device refuses inside its interrupt handler, no member's queue is told; each finds out at its next deadline. Item 7 of the same file.
- A test of the Periodic fallback when a device refuses to go one-shot (`docs/repairs/261010/kernel-time.md` item 3), which this does not touch.

**How it was found**
Not a defect found. The Dpc's failure was found by running the test: the lockup report named CPU 3, "took no tick for 10 s", and its thread, which was the dbg image's logger, not the Dpc worker.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can the replacement not be left to a worker thread, and what does the CPU lack at that moment that the worker needs?
2. Why is the table of enrolled devices an array that only grows, and what would go wrong with reading the mutex-guarded list from an interrupt handler?
3. Why does `device_refused()` loop, and what ends the loop?

## Three names that pointed at the wrong thing: `arm_or_park()`, `on_device_refused()`, `TickBroadcast::for_device()` (2026-10-11)

**Files**
- `kernel/include/ours/time/clock_event_device.hpp`, `kernel/kernel/time/clock_event_device.cpp` — `arm_or_park()`, the step that arms a one-shot device or parks it, now a member of the device it works on; it was `TickDevice::program_oneshot(dev, expired)`, a static function taking the device as an argument
- `kernel/include/ours/time/tick_device.hpp`, `kernel/kernel/time/tick_device.cpp` — `program_oneshot()` removed; `program_event()` calls `m_clkdev->arm_or_park()`; `device_refused()` is `on_device_refused()`
- `kernel/kernel/time/tick_broadcast.cpp`, `kernel/include/ours/time/tick_broadcast.hpp` — `m_device->arm_or_park()`; `of()` is `for_device()`
- `kernel/kernel/time/timer_queue.cpp`, `kernel/commands/tick_replace_test.cpp`, `kernel/commands/tick_broadcast_test.cpp` — the new names
- `docs/repairs/261010/kernel-time.md`, `.agents/skills/ours-kernel-design/references/task.md`, `.agents/tasks/high-res-timers/DESIGN.md` — the new names where they name the current code; older change records keep the names they were written with

**Symptom**
None, the behaviour is the same. The review of this branch (`.agents/tasks/high-res-timers/REVIEW.md`, "Minor and style") found the names awkward, and the awkwardness had a cause.

**Cause**
`program_oneshot(dev, expired)` is an operation on a clock event device (wake it if it sleeps, arm it, or stop it for "nothing pending") that sat on `TickDevice` only because `TickDevice::program_event()` was its first caller. The broadcast group needed the same step, so it called back into `TickDevice` for a function that had nothing to do with a CPU's tick: `tick_device.cpp` included `tick_broadcast.hpp` and `tick_broadcast.cpp` called into `TickDevice`, a two-way dependency between two classes that do not need each other for this. A function that takes an object as its first argument and uses nothing else of its own class is a member of that object's class in the wrong place; here that is the signal to move it. `device_refused()` is the reaction to an event, and its neighbours are named for the event they react to (`on_tick()`, `handle_oneshot_event()`). `of()` said nothing at the call site, where `TickBroadcast::of(*dev)` reads as a conversion; the function finds or makes the group that serves a device.

**Change**
Before: `TickDevice::program_oneshot(*m_device, earliest)` in the group, `program_oneshot(*m_clkdev, expired)` in `TickDevice`. After: `m_device->arm_or_park(earliest)` and `m_clkdev->arm_or_park(expired)`. The body is moved without change; `arm_or_park` is the name because a deadline of `INFINITE_TIME` parks the device and any other arms it. Ruled out: `program_oneshot` kept on `ClockEventDevice` under the same name, which still reads as "program in one-shot mode" when it also parks.

**Reference**
None, this project's own naming. `ClockEventDevice::program_event()` is the device-side entry the new member sits beside; Linux's `clockevents_program_event()` and `clockevents_switch_state()` are the two steps it combines.

**Verification**
- `ours-verify` on the build with the option off: dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS (144). On the build with `time_oneshot=true` both images PASS the same counts.
- `tickreplace.test`, `tickbcast.test`, `timer.test`, `timershare.test` and `sleep.test` on the one-shot build: all ok.
- `scripts/format_cpp.py --check` on every touched source: clean.

**Deliberately not done**
- The old names in earlier change records and in `REVIEW.md` are left, because they are history and the review is of that commit.
- No other change in this commit; the findings of the review are taken one at a time.

**How it was found**
By the code review of the branch; not a defect.

**The user's prediction**
None, a rename.

**Three questions you should be able to answer**
1. What made `program_oneshot()` a member of the wrong class, and which dependency between the two classes did moving it remove?
2. Why does a deadline of `INFINITE_TIME` park a one-shot device instead of shutting it down?
3. Why is a rename the one change that is not given a prediction first?

## A tick device that would not take the tick was chosen again for ever (2026-10-11)

**Files**
- `kernel/kernel/time/tick_device.cpp` — `replace_with()` marks the device that would not take the tick failed before it does anything else, and does not go back to the device it left when that one has failed; `on_device_refused()` no longer marks it afterwards, logs a warning instead of an error, and states why the loop ends
- `kernel/include/ours/time/tick_device.hpp` — the contract of `replace_with()` says so
- `kernel/commands/tick_replace_test.cpp` — a third case, and the header comment no longer describes the worker thread this design started with

**Symptom**
Latent, cannot be triggered with the devices in the tree today: it needs a device that takes deadlines at first and then stops taking the one-shot mode. With one (the test's `FadingDevice`), a refusal from another device sent the CPU into a loop with interrupts off. The log showed the same line, `CPU[3] tick device refusing refused a deadline`, 170 times, and then the lockup detector reported `cpu3 ... interrupts off, 1 spin locks held` and `cpu2 took no tick for 10 s` and stopped the machine.

**Cause**
Two functions, each reasonable alone.
- `on_device_refused()` loops: while the device in service has failed, choose the best device that has not failed and move the tick to it. It marked the chosen device failed only after `replace_with()` returned an error.
- `replace_with()`, when the new device would not take the tick, put the old device back and armed it again, so that the CPU was not left without a tick. In `on_device_refused()` the old device is the one that has just failed. It refused again, and that arrived at `on_device_refused()` a second time, from inside the first `replace_with()`.

The second call picked a replacement, and the device the first one was in the middle of trying was still not marked: it is marked after the call returns, and the call had not returned. So it picked the same device, which would not take the tick, put the failed device back, armed it, and so on. The measure that makes a loop like this end, the number of devices not yet failed, has to go down before anything that can come back into the loop runs, and here it went down after.

The loop's own comment argued that it ends ("each one failing at most once") and was right about the loop alone; the argument did not cover the call it makes, which re-enters it. A recovery path needs its termination argument written for the whole of what it calls.

**Change**
Before, in `replace_with()`:

```cpp
auto status = adopt(dev, group);
if (Status::Ok != status) {
    auto const back = adopt(left, left_group);   // `left` may have failed, and refuse again
    ...
    TimerQueue::get().rearm_device();            // refused: comes back to on_device_refused()
    return status;
}
```

After:

```cpp
auto const status = adopt(dev, group);
if (Status::Ok != status) {
    dev.mark_failed();                           // first: nothing below may choose it again
    if (left.is_failed()) {
        log::warn(...);
        return status;                           // nothing worth going back to
    }
    auto const back = adopt(left, left_group);
    ...
}
```

`on_device_refused()` stopped marking after `replace_with()` returns: `replace_with()` does it, on every failure, so the caller of `replace_with()` that is not `on_device_refused()` (`rebind_device()`, when a better device is enrolled and will not take the mode) gets a device that is not chosen again either.

The measure is now this. A device that is chosen is put in service or is marked failed before `replace_with()` returns; a failed device is never chosen; so each round that does not end the loop, and each call nested through `rearm_device()`, leaves one device fewer to choose from, and the list is finite.

Rejected: a flag in `TickDevice` that says a refusal is being handled and makes the nested call only mark and return. It also ends the loop, but by a rule about who calls whom and not by the data; the next path that re-enters would need the flag too.

**Reference**
Linux bounds the same kind of retry with a number, not with a state: `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) goes back to `retry` at most three times (`++retries < 3`), then gives up on the interrupt and sets `hang_detected`. Neither it nor `clockevents_program_min_delta()` (`kernel/time/clockevents.c`) has anything to hand the work to when a device refuses, which is why this tree's replacement path exists at all. Zircon has none.

**Verification**
- `tickreplace.test` has a third case that reproduces it: a device rated 500 takes the CPU, stops taking the mode, and a device rated 1000 that refuses everything is given to the CPU. Before the change: no verdict line, the lockup report above. After: 12 checks ok; the log shows `refusing` refused, `fading would not take the tick`, and the CPU ends on its own device with `fading` marked failed.
- `ours-verify` on the build with the option off and with `time_oneshot=true`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144).
- `tickbcast.test`, `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test` and `waitqueue.test` on the one-shot build: all ok.
- `scripts/format_cpp.py`, `clang-tidy -p out/bringup.x86` on `tick_device.cpp` and the test: no diagnostic in them.
- Not measured: a real device that stops taking a mode.

**Deliberately not done**
- A CPU left with no device at all still has no tick; that is item 6 of `docs/repairs/261010/kernel-time.md`.
- The log lines of a nested replacement come out in the wrong order (`tick moved from fading to refusing` is written after the replacement that ended the chain). A separate change.
- The retirement policy (one refusal retires a device) is the next change; this one does not touch what counts as a refusal.

**How it was found**
By the code review of this branch, which read the two functions side by side and asked what ends the loop when the call it makes comes back into it (`.agents/tasks/high-res-timers/REVIEW.md`, H1). Then reproduced with a probe device that took the mode and then stopped, run through `scripts/kshell_run.py`: the repeated line and the lockup report, with `cpu3 thread tickreplace ... interrupts off`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Which number goes down on every round of `on_device_refused()`, and why did marking the device after `replace_with()` returned come too late for it?
2. Why is putting the old device back, which is right when a better device was refused, wrong when the old device is the one that failed?
3. Why is a flag that stops the nested call a weaker fix than marking the device first?

## A broadcast group no longer takes its handler off the device it shares (2026-10-11)

**Files**
- `kernel/kernel/time/tick_broadcast.cpp`, `kernel/include/ours/time/tick_broadcast.hpp` — `join()` puts the group's handler on the device the first time any member joins and `leave()` of the last member stops the device but leaves the handler; the group records that it has installed it; the class comment says whose the handler is and why
- `kernel/include/ours/time/clock_event_device.hpp` — `has_event_handler()`
- `kernel/commands/tick_broadcast_test.cpp` — a check that the handler is still on the device once the last member has left

**Symptom**
Latent, cannot be triggered today and not reproduced: a window of a few stores, between the last member of a group leaving on one CPU and the global device interrupting on another. Its effect would be a call through an empty or half-moved handler on the CPU the device interrupts (the boot CPU, for the HPET).

**Cause**
The device's handler is a `ustl::function::Function`, a move-only callable with its target stored inline. `take_event_handler()` moves it out, which is several stores: the target's bytes are moved to the destination, the source is destroyed, and the source's table of operations is set to the empty one. `ClockEventDevice::handle_event()` reads it without any lock: it tests that there is a handler and then calls through it.

For a CPU's own device that is safe. The device interrupts the CPU that swaps, and the swap is made with interrupts off, so the interrupt cannot land in the middle of it (that is the repair of 260926). A group's device is not any CPU's own: the HPET interrupts the boot CPU whichever CPU is leaving. Interrupts off on the leaving CPU stop nothing on the boot CPU, which can read the handler at any point in the stores. The group's lock does not help either: the handler takes it only after it has been called.

There is a second route to the same fault in the same call. `leave()` can run inside the handler it removes: `on_device_event()` runs a member's queue, a queue's end can find a refused deadline, and that ends in `on_device_refused()`, `release()` and `leave()` of the last member, which destroyed the closure of the call that was still executing. It worked because the closure only holds `this` and nothing touches it after the call returns.

**Change**
Before, in `leave()`: `auto dropped = m_device->take_event_handler();` then `switch_mode(Detached)`; in `join()`: `set_event_handler(...)` for every first member, and `take_event_handler()` again if the mode would not take. After: the handler is set once, by the first join the group ever has, and nothing takes it off.

```cpp
DEBUG_ASSERT(m_device->has_event_handler() == m_handler_installed,
             "{} has an owner other than its broadcast group", m_device->name());
if (!m_handler_installed) {
    m_device->set_event_handler([this](ClockEventDevice *) { on_device_event(); });
    m_handler_installed = true;
}
```

With no member the device is stopped, and an interrupt that was already on its way finds nobody whose deadline has come and returns. The assertion states the rule that makes this correct: while a group has the device, nobody else puts a handler on it. A device that has another use is another comparator, which has a slot of its own, or a member of the group (`.agents/tasks/high-res-timers/DESIGN.md`, Decisions).

Rejected: waiting for the other CPUs' handlers to finish before the swap, which wants a primitive to wait on them that the interrupt layer does not have; and publishing the handler as one pointer to an immutable record, which is the right shape if a device's handler has to change hands at run time (Linux's `event_handler` is a single function pointer), but changes the type of every handler in the tree for a case that does not exist.

**Reference**
Linux keeps `event_handler` as a plain function pointer on the `clock_event_device` and rewrites it in `tick_set_periodic_handler()`, `tick_install_broadcast_device()` and `tick_broadcast_setup_oneshot()` (`kernel/time/tick-common.c`, `kernel/time/tick-broadcast.c`); a one-word store is not torn the way a `Function` is. Zircon has no broadcast group.

**Verification**
- `tickbcast.test` has a new check, that the handler is on the device after the last member has left. It and `tickreplace.test` were each run twice in a row on the one-shot build, so the second pass joins a group whose handler is already there: all ok.
- `ours-verify` on the build with the option off and with `time_oneshot=true`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144). `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test`, `waitqueue.test`: ok.
- `scripts/format_cpp.py`, `clang-tidy -p out/bringup.x86` on the touched sources: no diagnostic in them.
- Not reproduced and not measured: the race itself. The window is a few stores wide and QEMU's TCG interleaves CPUs in large steps; the argument rests on the stores above.

**Deliberately not done**
- A device whose handler has to change hands at run time. Nothing needs it, and the first thing to do when something does is to publish the handler as a single pointer.
- `TickDevice::release()` still moves the handler out of a CPU's own device. That is the case the interrupts-off rule covers.

**How it was found**
By the code review of this branch, reading `take_event_handler()` against `handle_event()` and asking which CPU runs each (`.agents/tasks/high-res-timers/REVIEW.md`, M1). Read-only; the user asked for the scope of the HPET to be settled first, and the decision is in `DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is a swap of a device's handler with interrupts off safe for a CPU's own device and not for the HPET?
2. What stores does moving a `Function` consist of, and what can a CPU reading it between two of them see?
3. What does the group's handler do when an interrupt arrives after the last member has left?

## A tick device is asked three times before it is retired (2026-10-11)

**Files**
- `kernel/kernel/time/tick_device.cpp`, `kernel/include/ours/time/tick_device.hpp` — `on_device_refused()` asks the device again twice before marking it failed; `ATTEMPTS_BEFORE_RETIRING`
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp` — `try_rearm_device()`, the arming of `rearm_device()` that answers instead of reporting the refusal
- `kernel/commands/tick_replace_test.cpp` — a fourth case, and the count of times the first case's device was asked
- `.agents/tasks/high-res-timers/DESIGN.md` — the decision

**Symptom**
Not a defect; a policy. A tick device that refused one deadline was retired for good, and a refusal of the kind the HPET gives when the comparator write may have landed too late can be a passing thing under emulation, where the host can stall a vCPU between the write and the read of the counter. Retiring the HPET on one such refusal takes away the only global device, which is what every CPU falls back to.

**Cause**
The first design of the replacement (earlier today) treated a refusal as proof: `ClockEventDevice::program_min_delta()` had already asked ten times at growing deltas, so a refusal that reached the queue seemed to mean the device was gone. That is true of a device that has failed and not of one that was unlucky, and the two cannot be told apart from one answer. No way back existed (`clear_failed()` is for tests), so the cost of being wrong was permanent. The user decided on a retry policy (2026-10-11).

**Change**
`on_device_refused()` now asks again, back to back, and retires the device only when every ask is refused:

```cpp
for (u32 attempt = 1; attempt < ATTEMPTS_BEFORE_RETIRING; ++attempt) {
    if (TimerQueue::get().try_rearm_device()) {
        log::info("... took the deadline at attempt {}", ...);
        return;
    }
}
device.mark_failed();
```

`try_rearm_device()` arms the device afresh for the queue's earliest deadline, returns whether it took it, and consumes the queue's refusal flag itself, so a refusal of one of these asks is counted (`timer.queue.program_refused`) but does not come back to `on_device_refused()`. It is a loop of two and not a recursion, which keeps the termination argument of the earlier entry the same.

What this does and does not cover, said plainly: the asks are back to back. Each is itself the device layer's ten attempts at growing deltas, with the clock re-read every time, so the three asks give the device thirty tries in a few microseconds. That covers a refusal that clears within microseconds. It does not cover one that lasts a millisecond, because there is nothing to wait with: a CPU whose device has just refused has no timer, and a spin of a millisecond with interrupts off for a rare event was not judged worth building without a device that is known to need it.

Ruled out: retiring and probing again after a while, which needs a timer on the CPU that has none; and a counter in the device that accumulates refusals across time, which needs a reset on every success, a store on the path of every programming.

**Reference**
Linux bounds the same thing by count and gives up after it: `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) goes back to `retry` while `++retries < 3`, and `clockevents_program_min_delta()` (`kernel/time/clockevents.c`, the variant without `CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST`) tries ten times at a growing delta, then returns `-ETIME`, which is what `ClockEventDevice::program_min_delta()` is modelled on. Neither asks the same device again after that, because Linux has no other device to move the tick to. Zircon has none.

**Verification**
- `tickreplace.test` has a fourth case: a device that refuses its first two programs and takes the third keeps the tick, is not marked failed, and the queue counts exactly two refusals; the first case now checks that a device that refuses every time is asked exactly three times. 16 checks ok. The log shows `refused a deadline`, then `took the deadline at attempt 3`.
- `ours-verify` on the build with the option off and with `time_oneshot=true`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144). `tickbcast.test`, `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test` and `waitqueue.test` on the one-shot build: all ok.
- `scripts/format_cpp.py`, `clang-tidy -p out/bringup.x86` on the touched sources: no diagnostic in them.
- Not measured: a real device whose refusal clears; the fake clears by the count of refusals, not by time.

**Deliberately not done**
- A wait between the asks, and a later probe of a retired device. Neither has a device to justify it; the first thing to add when one does is a short spin before the last ask.
- The number three is a guess, kept as a named constant. No device in the tree refuses.

**How it was found**
From the review of this branch (`.agents/tasks/high-res-timers/REVIEW.md`, M2) and the user's decision.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can a refusal that reaches the timer queue not tell a failed device from an unlucky one?
2. Why does `try_rearm_device()` take the refusal flag itself, and what would happen if it left it for `notify_refusal()`?
3. What kind of refusal does asking back to back cover, and what kind needs a wait that this kernel has no way to do?

## A thread that moved CPUs while it armed or cancelled a timer asked the wrong queue about a refusal (2026-10-11)

**Files**
- `kernel/kernel/time/timer_queue.cpp` — `activate_timer()` keeps interrupts off from the choice of the queue to the notify at the end
- `kernel/kernel/time/timer.cpp` — `Timer::cancel()` does the same from the removal to the notify

**Symptom**
Latent, cannot be triggered with the devices in the tree today, and not reproduced. Two windows, each a few instructions wide, in thread context.
- In `activate_timer()`, `TimerQueue::get()` was read before the queue's lock was taken. A thread preempted there and resumed on another CPU locked the queue of the CPU it had left. When that made the timer the head, `reprogram_locked()` programmed the device of the CPU it was now on for the old queue's deadline. A debug build stops on the assertion `Reprogramming another CPU's tick device`; a release build does the wrong thing.
- In both, the notify at the end asked `notify_refusal()` of the queue of the CPU the thread was on then. If the thread had moved after the removal or the arming, that was another queue, and the queue whose device had refused kept its flag up. `notify_refusal()` leaves a flag it cannot act on for the next call on its own CPU, and a CPU whose device has just refused has no timer interrupt, so nothing guarantees a next call: the CPU could stay without a tick.

**Cause**
`Guard<SpinLock, IrqSave>` keeps a thread on its CPU for as long as it is held, but the choice of which queue to lock was made outside it. "The current CPU" is an answer that is only good while nothing can move the thread, and a per-CPU lookup followed later by a lock is two steps with a gap between them. The same shape in `Timer::cancel()`: the removal is under the lock, the question afterwards is not.

**Change**
An `arch::AutoInterruptDisabler` covers the lookup, the lock and the notify:

```cpp
arch::AutoInterruptDisabler const no_interrupts;
auto &self = TimerQueue::get();
```

and, in `Timer::cancel()`, the removal and the `notify_refusal()` in a block of their own, ending before the spin that waits for a handler on another CPU, which has to stay interruptible. Interrupts off is the tool here because a thread is only moved when it is preempted, and that happens at the return from an interrupt.

Rejected: `AutoPreemptDisabler`, which is for thread context, while `activate_timer()` is also called from timer handlers; and having `notify_refusal()` forward a flag it cannot act on to the CPU it belongs to by IPI, which is more machinery for a case that disappears when the thread cannot move.

**Reference**
Zircon `Timer::Set()` and `Timer::Cancel()` (`zircon/kernel/kernel/timer.cc`) take the queue's lock with interrupts disabled, `Guard<MonitoredSpinLock, IrqSave>`, from the start of the function, and read the current CPU's queue inside it. Linux `hrtimer_start_range_ns()` (`kernel/time/hrtimer.c`) takes the base's lock first (`lock_hrtimer_base()`) and finds the CPU from the base it locked.

**Verification**
- `ours-verify` on the build with the option off and with `time_oneshot=true`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (144). `tickbcast.test`, `tickreplace.test`, `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test` and `waitqueue.test` on the one-shot build, and the last four on the default build: all ok.
- `scripts/format_cpp.py`, `clang-tidy -p out/bringup.x86` on `timer.cpp` and `timer_queue.cpp`: no diagnostic in them.
- Not reproduced: the windows are a few instructions wide, and the thread has to be preempted in one of them. Not measured: the cost of the interrupt disable, which is a `cli` and a restore on a path that takes a spin lock with interrupts off anyway.

**Deliberately not done**
- Other places that read a per-CPU object and lock it later. This change is the two the review found; a search for the shape across the tree is not part of it.

**How it was found**
By the code review (`.agents/tasks/high-res-timers/REVIEW.md`, M3), which found the second window; reading `activate_timer()` to fix it showed the first, which is older than this branch.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is `Guard<SpinLock, IrqSave>` not enough when the queue is chosen before it is taken?
2. Why must the spin that waits for a handler on another CPU stay outside the block with interrupts off?
3. Why is a refusal flag left up on a queue whose CPU has no timer interrupt a problem that waiting does not solve?
