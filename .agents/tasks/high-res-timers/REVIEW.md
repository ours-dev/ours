# high-res-timers: review of the branch feat/high-res-timers (8ab9400b..b5a19149)

**Date**: 2026-10-11

## Scope and method

Eleven commits, 31 source files under `kernel/` (+2315, -161): the one-shot tick and its boot option, `TickSched` and `TickDevice` after the refactor, the timer queue's refusal protocol, the new `TickBroadcast` and the timer IPI, the x86 pieces (`stop_oneshot()` of the local APIC timer and of the HPET, `IrqVec::IpiTimer`), and four console tests. Documents were not reviewed.

Authorities read: `modern-cpp-development`, `ours-kernel-design` (`references/task.md`, Time), `.clang-format`, `.clang-tidy`, and the surrounding files. Linux counterparts were read in the local tree (`kernel/time/tick-broadcast.c`, `clockevents.c`, `hrtimer.c`); Zircon has none for the broadcast group.

Evidence:
- `clang-format --dry-run -Werror` on the six new files: clean. `scripts/format_cpp.py --check` on every touched file: clean.
- `clang-tidy -p out/bringup.x86` on ten touched sources: no diagnostic in any of them except two old member-name warnings in `hpet.cpp` (`id_`, `regs_mmio_`), not in changed lines.
- `ours-verify` on the default and on the `time_oneshot=true` build: both images pass; the sequence `tickreplace.test`, `tickbcast.test`, `timer.test`, `timershare.test`, `sleep.test`, `semaphore.test`, `waitqueue.test` passes.
- **Reproduced:** H1, with a throwaway probe command (not committed): 170 repetitions of the refusal line, then the lockup detector's panic.
- Everything else below is **read-only judgement** and says so.

## What is done well

- `kernel/kernel/time/clock_event_device.cpp`, the enrolled-device table in `enroll()` and `for_each_enrolled()`: one writer (under `GCED_MUTEX`), the entry stored relaxed and the count stored release, the reader loading the count acquire. The pairing is right, minimal, and the reason (a CPU must choose a replacement where the mutex cannot be taken) is in the comment.
- `kernel/kernel/time/tick_broadcast.cpp:148-175`, `on_device_event()`: the due members are taken out of the books, the device is re-armed for the rest, and only then, with the lock dropped, the local queue runs and the IPIs go. A queue that takes long therefore never delays another member's deadline, and neither lock order can invert. The order (queue lock, then group lock) is stated in the class comment and followed in `set_deadline()`.
- `kernel/kernel/time/timer_queue.cpp:70-114`: one sentinel for "the device may hold anything" (`UNKNOWN_DEADLINE`), used the same way in `TimerQueue` and in `TickBroadcast::rearm_locked()`; `notify_refusal()` costs one load on the common path and re-checks, under interrupts-off, that it is still on the queue's CPU.
- After 16af5198 each fact has one owner: `TickSched::m_started` is whether the scheduler exists, the device's mode is how the tick is produced, and `on_tick()` reads the second instead of keeping a copy.
- The tests are written to fail when the thing breaks: `tickbcast.test` asserts that IPIs were sent, `tickreplace.test` fails if the CPU does not end on the device it should, and a flake in `timershare.test` was traced to its statistic (f7b2bfd3) and not loosened away.
- Comments in the changed sources do not name Linux or Zircon (checked with a search over the added lines), as CLAUDE.md asks.

## High severity

### H1. The recovery path can fail to terminate: `replace_with()` puts a failed device back and re-arms it

`kernel/kernel/time/tick_device.cpp:374-388` and `:413-427`. **Reproduced.**

When `adopt()` of the replacement fails, `replace_with()` re-adopts the device it left (`left`), then calls `rearm_device()`. In `device_refused()` that `left` is the device that has just failed. The sequence:

1. `device_refused()` on CPU c: the device in service, F, is marked failed. `pick_replacement()` names D, the best device that has not failed.
2. `replace_with(D)`: `release()` lets go of F; `adopt(D)` fails (say D's `set_mode_oneshot()` returns an error).
3. The failure branch calls `adopt(F)`, which succeeds (F refuses deadlines, not modes), then `TimerQueue::rearm_device()`.
4. F refuses again, so `notify_refusal()` calls `device_refused()` a second time, nested. Its loop calls `pick_replacement()` again, which still returns D: the line that marks D failed, `pick.device->mark_failed()` at `:425`, runs only after `replace_with()` returns, and it never does. Back to 2.

Probe: a device rated 500 whose `set_mode_oneshot()` fails, enrolled on CPU 3 (it is refused and CPU 3 stays on its local APIC timer), then the rating-1000 device that refuses every deadline. Result: the line `CPU[3] tick device refusing refused a deadline` repeated 170 times, then `lockup: cpu2 took no tick for 10 s` and the panic. No device in the tree fails to take a mode, so nothing reaches this today; it is the code that exists for the day something does.

Why it matters beyond the case: the loop's comment argues termination ("each one failing at most once"), but the measure it relies on, the number of devices not yet failed, is decremented too late, after the call that can re-enter.

Fix, in this order of preference: (a) in `replace_with()`'s failure branch mark `dev` failed before anything else, and do not restore `left` when it is itself failed (a failed device is not worth re-arming; say so as an error and leave the CPU without a tick, as `device_refused()` already does when nothing is left); (b) a re-entrancy flag in `TickDevice` so that a nested `device_refused()` marks and returns, leaving the outer loop to continue. Then add the probe as a third scenario of `tickreplace.test`, which would have caught it.

## Medium severity

### M1. A data race on the HPET's event handler when the last member leaves a group

`kernel/kernel/time/tick_broadcast.cpp:94-98` (and the mirror image in `join()`, `:71`). **Read-only judgement; not reproduced.**

The group's device is not a CPU's own: its interrupt is delivered to the boot CPU whatever CPU is leaving. `leave()` runs on CPU k, under the group's lock and with k's interrupts off, and does `take_event_handler()` then `switch_mode(Detached)`. Meanwhile the HPET can fire on the boot CPU, which runs `handle_event()` (`kernel/include/ours/time/clock_event_device.hpp:195-199`: `if (m_event_handler) m_event_handler(this);`) before it reaches the group's lock. The move of a `ustl::function::Function` is several stores (the target relocated and its source destroyed, then the source's ops pointer set to the empty table, `function.hpp:161-183`), so the boot CPU can see the ops pointer of a handler whose storage is half moved.

The fix of 260926 (interrupts off for the swap) covers the case where the device interrupts the CPU that swaps. It does nothing for an interrupt on another CPU, which is the case a group creates.

The same call has a second problem on a different path: `leave()` can run inside the handler it removes. `on_device_event()` runs the member's queue, whose end can call `notify_refusal()`, `device_refused()` and so `release()` and `leave()`, which destroys the closure of the very call that is executing. It is safe today only because the closure is `[this]`, trivially destructible, and nothing touches the `Function` after the call returns.

Fix: install the group's handler once, at the first `join()` of the group's life, and never take it out. `leave()` of the last member then only quiets the device (`switch_mode(Detached)` masks the comparator), and a stray interrupt finds no member due and returns. No `Function` is mutated while any CPU can call it.

### M2. One refusal retires a device for good: a policy the user should decide

`kernel/kernel/time/tick_device.cpp:405` (`mark_failed()` in `device_refused()`).

A refusal that reaches the queue has already been retried ten times by `program_min_delta()`, which is the case for treating the device as gone. But the HPET's "too close" answer is the one refusal that can be transient, and under QEMU's TCG a host stall can produce it. Retiring the HPET permanently takes away the only broadcast fallback for every CPU, and if it is the group's device, breaks the members that are using it (repair item 7). There is no way back except `clear_failed()`, which only tests call. The change record states the policy, and the alternatives (N refusals in a row; probe and readmit after a while) are not written down anywhere. This is a trade-off between availability and a flapping device, so the decision belongs to the user, not to the author; it was made here without being asked.

### M3. A refusal can be left unreported

- `kernel/kernel/time/timer_queue.cpp:107-109`: a caller that migrated between the reprogram and `notify_refusal()` leaves `m_refused` set "for the next visit to this queue's CPU". On a CPU whose device has just refused there is no timer interrupt, so nothing guarantees a next visit. The window is small (a thread-context `activate_timer()` or `cancel()` that is preempted after dropping the lock), but the consequence is a CPU that stays tickless until something else arms its device. Keep the thread on its CPU (a preemption disabler around the lock and the notify) in the two thread-context sites, `activate_timer()` and `Timer::cancel()`.
- A refusal inside the group's own interrupt handler reaches no member. This is `docs/repairs/261010/kernel-time.md` item 7; agreed, and Medium is the right severity.

## Minor and style

Naming first, as asked:

- `TickDevice::program_oneshot()` (`tick_device.cpp:86`) is an operation on a `ClockEventDevice` (arm it, or park it), hosted on `TickDevice` only so that `TickBroadcast` can call it. It makes the two classes depend on each other: `tick_device.cpp` includes `tick_broadcast.hpp`, and `tick_broadcast.cpp` calls into `TickDevice`. Move it onto `ClockEventDevice` (`arm_or_park()`, say) and the cycle disappears. This is the "suspect the API" question: the helper is awkward because it is in the wrong class.
- `device_refused()` is an event handler, and the neighbours are `on_tick()`, `handle_*`: `on_device_refused()`. `TickBroadcast::of()` says nothing at the call site: `for_device()`.

Other:

- `tick_device.cpp:367-369`: two ternary operators in `replace_with()`'s `DEBUG_ASSERT` message break the no-ternary rule of `modern-cpp-development`. They only run in debug builds, which is where a reader would trip over them. Give the message fewer moving parts.
- In group mode `TickDevice` keeps the device twice, in `m_clkdev` and in `m_group->device()`, with no check that they agree: the "one owner per fact" the refactor was about. Derive `device()` from `m_group`, or `DEBUG_ASSERT(&group->device() == &dev)` in `adopt()`.
- `clock_event_device.hpp:208-212`: `mark_failed()` promises that a failed device is never chosen by `TickDevice::check_new_device()`, but `is_failed()` is read only at `tick_device.cpp:413` and `:441`. Nothing can enrol a failed device today, so it is harmless, but the sentence is not true of the code; make it true or drop it. The comment is also wrapped badly in the middle (the formatter does not rewrap comments).
- The nested replacement logs in the wrong order. `replace_with()` writes "tick moved from X to Y" after `rearm_device()` returns (`:390-391`), and the inner replacement finishes first, so `tickreplace.test` shows "moved from refusing to LVT" before "moved from LVT to refusing". `setup_oneshot_tick()` likewise logs "runs one-shot" (`:314`) for a device that may already have been replaced. Log before the re-arm, or log the device in service afterwards.
- Test-only entry points now sit in production headers: `TimerQueue::num_refused_programs()`, `TickBroadcast::num_ipis_sent()`, `num_refused_programs()`, `has_member()`, `ClockEventDevice::clear_failed()`. Acceptable and documented; they widen the API for tests, and a general counter dump (none exists) would remove most of them.
- `Fired` and the spin-wait `sample()` are written three times across `timer_test.cpp`, `tick_broadcast_test.cpp` and `tick_replace_test.cpp`. Fine at three; extract at the fourth.

## Teaching opportunities

### T1. Recovery code needs its own termination argument

H1 and the Dpc that failed on its first run (recorded in `docs/changes/261011/kernel-time.md`) are the same lesson from two sides. Code that runs when something has already failed runs with the usual assumptions off, so it has to say why it ends and what it depends on.

- The Dpc depended on the scheduler, and a CPU that has just lost its only timer device has nothing to preempt a thread with, so the worker waits behind whatever the CPU is running, for ever if that thread never blocks.
- The loop in `device_refused()` depends on a count (devices not yet failed) going down on every round, and the count is lowered in the wrong place.

Write the measure down next to the loop, then check every path that re-enters it. Linux's `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) does it with a literal bound: `if (++retries < 3) goto retry;`, then a hang flag. A re-entrancy flag (H1, option b) states the same thing in the structure of the code instead of in an argument about depth.

### T2. Never mutate a callable that another CPU can call

M1, with the interleaving:

1. CPU k, the last member, holds the group's lock and calls `take_event_handler()`: the closure bytes are moved into a temporary, the source is destroyed, and the source's ops pointer is stored as the empty table.
2. The HPET fires on the boot CPU, which does not take the group's lock until it is inside the handler. It runs `if (m_event_handler) m_event_handler(this)`: it reads the ops pointer, then calls through it with the storage address.
3. Any interleaving of 1 and 2 where the boot CPU's loads fall between the stores of CPU k reads a handler that is half moved, or destroyed while being called. For `[this]` the bytes survive and nothing visible happens; for a closure with a non-trivial destructor it is a use after destroy ([basic.life] at cppreference, "Object lifetime").

Interrupts off is a same-CPU tool. It stops the interrupt that is on the CPU doing the swap, and it is why 260926's fix works for a CPU's own device. It stops nothing on another CPU. The fix pattern is to make the thing the other CPU reads immutable after publication (install once) or to quiesce the source before mutating, and a lock is the wrong tool unless the interrupt side takes it too.

### T3. Memory orders: say who pairs with whom

- `NUM_ENROLLED` (release store, acquire load) pairs a writer with readers who need the entries: correct.
- `m_started` (release in `start()`, acquire in `is_started()`) publishes a scheduler built by another CPU: correct.
- `TimerQueue::m_refused` (`timer_queue.cpp:92, 100, 110`) is written and read by one CPU, thread against its own interrupt handlers. What it needs is atomicity against an interrupt, not ordering with another thread. The acquire and release orderings have no partner, which is harmless and misleading, because a reader will look for the pairing. Use relaxed and say why.
- `ClockEventDevice::m_failed` is written by one CPU and read by others, but publishes nothing else; the orderings are again unneeded. If it ever carries a reason beside it, release becomes necessary.

The habit to take: for each atomic, write down the data its release publishes, or write "none, relaxed". The one-line answer is the review of the ordering.

## Suggested follow-up order

1. Names: move `program_oneshot()` onto `ClockEventDevice`, `device_refused()` to `on_device_event_refused()`/`on_device_refused()`, `of()` to `for_device()`. Later edits then land on the right names.
2. H1: mark the candidate failed in `replace_with()`'s failure branch, do not restore a failed device, and add the probe as scenario three of `tickreplace.test`.
3. M1: install the group's handler once; `leave()` only quiets the device.
4. M2: decide the retirement policy with the user and write the decision into `DESIGN.md`.
5. The ternaries in `replace_with()`, the `mark_failed()` sentence and its wrapping, and the log order.
6. M3: pin the thread in the two thread-context sites of `notify_refusal()`.
7. `m_refused` and `m_failed` to relaxed with a sentence each; the derived `device()` in group mode.
