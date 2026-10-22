# idle-power: an idle CPU is woken about once a second, and waits in MWAIT when it can

**Date**: 2026-10-23

## Delivered
- bd20329a: per-CPU counts of timer interrupts, halts and time halted, and `idle.stats` / `idle.test` (step 1); 31dab8f8: the time halted counts the part already gone of a halt in progress.
- e393affb: the two sample threads that kept two CPUs busy for ever are no longer started (the user's choice ii).
- c61f59a5: the lockup heartbeat is a per-CPU one-second `Timer`, not the tick (step 2); c8e22974: its notes and report say beats, not ticks.
- c6764d82: a slice ends at its CPU's preemption deadline, a slot in the timer queue that the scheduler sets at every pick and empties for the idle thread; the tick no longer calls the scheduler; `preempt.test` (step 3). Fixed `docs/repairs/261009/kernel-time.md` item 2.
- 1ede96ab: the tick `Timer` is gone; a one-shot CPU is armed only for its timers and preemption deadline, and jiffies advance only from a periodic tick (step 4, decision A); c65a75cc: `TickSched` loses the two unused fields of a tick stopped while idle.
- d3ea77c8: with MONITOR, an idle CPU waits in MWAIT on a per-CPU word, and a reschedule to it writes the word instead of sending an IPI (step 5).
- 5c2b1388 and 4ddf6794: the `idle_states` library, a table per processor model cut to the states CPUID leaf 5 reports, and the governor `IdleStates::pick()` that chooses the state each wait enters, C1 alone enabled (step 6, scope A).

## Verification
- Every step: built and booted in `.worktrees/wip-proc-self` at the step before plus only that step, clang-tidy clean on its lines, and mutations its tests catch.
- At b9483945, after twelve later commits by other sessions, four of them in `timer_queue.cpp`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 146 passed; `idle.test`, `preempt.test` and `hotplug.test` (40 checks) ok.
- Timer interrupts per idle CPU per second, `idle.stats` on a quiet dbg image: about 100 at the start (the tick, with two CPUs never idle at all); 101 after step 2 (tick and heartbeat); 0.9 to 1.3 after step 4 (the heartbeat, and on CPU 0 the measuring thread's own sleep). Time halted 99.9% throughout.
- Slices end at the deadline: `preempt.test` turns of 65.2 to 65.7 ms against a 64 ms slice, 70 ms on the periodic build.
- MWAIT, under `CPU=Nehalem,+monitor` and `Skylake-Client,+monitor`: every test above passes; 20 to 38 of 100 wakes of an idle CPU end by a write rather than an IPI.
- Not measured: power. QEMU's TCG runs MWAIT as a PAUSE with more than one CPU, and its CPUID leaf 5 reports no C-states, so neither the saving nor a deep state can be observed here; the governor is tested on the host only (21 tests).

## Deviations from the design
- The governor bounds Zircon's guess by the time until the next deadline. Zircon uses the last idle stretch alone; the bound was chosen for the user, who did not answer, and is theirs to overrule.
- Step 7, states that stop the local APIC timer handed to `TickBroadcast`, was not built (scope A); it is a repair item.
- The design's deep-idle governor was to be the milestone's hand-written function; the user declined ("不手写", 2026-10-14).

## Found along the way
- `CpuLocal::Current<T>::get()` returned a copy of any `T` of 8 bytes or less, class types included, so a change through `get()` landed on the copy; fixed in 93e4b037, found by deleting two fields from `TickSched`.
- A CPU going offline moved threads to other CPUs without waking them, so an idle target, with no tick, slept up to a second (637 and 777 ms measured); fixed in c2960d4f, with `hotplug.test`'s `check_moved_to_idle()`.
- The first governor commit wrote the state table at `InitLevel::Platform`, when every CPU was already reading it; moved before the APs start in 4ddf6794.
- The user's review of step 2 reshaped the offline path: subsystems join a CPU's going and coming through registered hooks (36e8e889), and the run queue walks its own threads for the move (54203477, 3990d775, 7a5e86db).
- Every pick of a thread now takes the timer queue's lock and writes the timer device: `docs/repairs/261013/kernel-time.md` item 1, a performance item for the user.

## Not done
- States that stop the local APIC timer on CPUs without ARAT: `docs/repairs/261014/x86-idle.md` item 1.
- Removing jiffies, with the local APIC's calibration timed by the HPET: `docs/repairs/261013/kernel-time.md` item 2.
- States deeper than C1 enabled, and tables for models other than Skylake: nothing here can measure them, and Zircon ships C1 alone too.
- The three concurrency arguments in PLAN.md's Decisions, paused with all homework by the user on 2026-10-21.
- The user has not yet reviewed step 3's diff (c6764d82).
