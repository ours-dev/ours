# lockup-detector changes

## The heartbeat is a per-CPU timer of its own, once a second, not the tick (2026-10-13)

**Files**
- `kernel/lib/lockup_detector/src/checker.cpp` — `HEARTBEAT_TIMER` per CPU, `on_heartbeat()` (beat, check the others, re-arm one second on), `arm_this_cpu()`, armed on every online CPU by `arm()`; `on_tick()` and `TICKS_PER_CHECK` gone; `on_cpu_offline()`, `on_cpu_online()`
- `kernel/lib/lockup_detector/include/lockup_detector/lockup_detector.hpp`, `README.md`, `src/report.cpp` — the interface and the wording: beats, not ticks; the report says "had no heartbeat for <T> s"
- `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn` — the tick no longer calls the detector
- `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/BUILD.gn` — the CPU going offline cancels its heartbeat timer, the CPU coming back arms it
- `scripts/kshell_run.py` — the report header it matches

**Symptom**
None yet; step 2 of `.agents/tasks/idle-power/PLAN.md`. Once the periodic tick is gone, a heartbeat counted by the tick stops on every idle CPU, and every idle CPU would be reported stuck after `kernel.lockup.threshold` seconds.

**Cause**
`.agents/tasks/lockup-watchdog/DESIGN.md` D1 chose the tick as the heartbeat (option a) because every CPU ticked, and named the per-CPU one-second timer (option b, Zircon's) as the move to make when tickless idle came. It has come.

**Change**
The objects:
- **`HEARTBEAT_TIMER`**: per CPU, a constant-initialised `Timer` in CPU-local storage.
- **`HEARTBEAT`**: per CPU, the beats it has had.
- **`NOTES`** and **`THRESHOLD`**: as before.

The events:
1. At `InitLevel::Platform`, `arm()` stores the threshold and arms every online CPU's timer, each on its own CPU through `sync_exec()`.
2. Each second, on each CPU, `on_heartbeat()` moves `HEARTBEAT`, checks every other online CPU as `check_others()` always did, and returns Restart one period on.
3. A CPU going offline cancels its own timer before its tick stops, so it is not among the timers another CPU takes over. Coming back, it arms it again if the checker is on.

A check still counts seconds, so the threshold keeps its meaning. A stuck CPU is still one with interrupts off: its timer interrupt cannot come. The report is still made from the checking CPU's timer interrupt, whose frame `irq::current_frame()` gives.

The cost of moving off the tick: an idle CPU is woken once a second for its heartbeat. Zircon pays the same, with its lockup detector's one-second heartbeat timer.

**Reference**
- Zircon's lockup detector heartbeat (`zircon/kernel/lib/lockup_detector/lockup_detector.cc`), a per-CPU periodic `Timer` re-armed every period, one second by default.
- Linux's softlockup detector runs from a pinned per-CPU hrtimer, every four seconds by default (`kernel/watchdog.c`).

**Verification**
- Built in `.worktrees/wip-proc-self` at e393affb plus only this change; clang-tidy reports nothing new on the changed lines.
- `boot_check.py`: dbg and rel PASS.
- `kshell_run.py "lockup status" idle.test hotplug.test "lockup spin 2"` on dbg: all ok. `lockup status` shows one beat a second per CPU. `lockup spin 2` reads "cpu2 had no heartbeat for 10 s, found by cpu0". `hotplug.test` takes CPU 3 down and up with its heartbeat cancelled and re-armed, and no CPU is reported.
- `kshell_run.py "lockup spin all"`: ok, through the script's NMI after 15 s of silence.
- `idle.test`: 100 to 101 timer interrupts a second per idle CPU, the tick's 100 and the heartbeat's one, where they do not fall together.
- Mutation: with the heartbeat not re-armed, no CPU checks after its first beat, and `lockup spin 2` is never reported (FAIL at the command timeout).

**Deliberately not done**
- The tick itself stays for now: the next steps take its other consumers away first.
- The notes' `fewest_ticks` keep their name; they now count beats.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must the heartbeat leave the tick before the tick can go?
2. Why is a CPU whose timer interrupts stop still a CPU with interrupts off, and what other cause would look the same?
3. What happens to a CPU's heartbeat timer as it goes offline, and why is it cancelled rather than moved with the CPU's other timers?

## The detector's notes and report count beats, not ticks (2026-10-13)

**Files**
- `kernel/lib/lockup_detector/include/lockup_detector/heartbeat_note.hpp` — `fewest_ticks()` becomes `fewest_beats()`; comments
- `kernel/lib/lockup_detector/test/heartbeat_note_test.cpp` — test names and calls follow
- `kernel/lib/lockup_detector/src/checker.cpp`, `src/checker.hpp` — `m_fewest_beats`; comments
- `kernel/lib/lockup_detector/src/report.cpp`, `src/report.hpp` — the report says "checks in a row without a beat" and "N beats"; comments
- `kernel/lib/lockup_detector/BUILD.gn` — the target's comment

**Symptom**
The lockup report printed "checks in a row without a tick" and "11 ticks taken" for a count of heartbeats, and `HeartbeatNote` spoke of ticks throughout. Since the heartbeat moved to a one-second timer of its own (step 2 of idle-power) the two are not the same thing, and since step 4 a one-shot CPU has no tick at all, so the words named something that does not exist.

**Cause**
Step 2 renamed what the checker's own file said, and `lockup status`, but not the note it keeps per CPU, the report, or their headers; the step-4 run of `lockup spin 2` showed the old words in the report.

**Change**
Wording only, and one accessor: `HeartbeatNote::fewest_ticks()` is `fewest_beats()`, and what it counts is unchanged. The report's lines now read, from `lockup spin 2`:

```text
lockup: checks in a row without a beat, as cpu0 counted them: cpu1 0 cpu2 10 cpu3 0
lockup: cpu0 (reporting) thread Idle/0 at +0xb4cb6, interrupts on, 0 spin locks held, 11 beats
```

The README's sentence on the threshold data ("without a tick, when the tick was the heartbeat") is about measurements taken then and stays; its account of logz4's "ticks" is logz4's.

**Reference**
None, wording.

**Verification**
- Built in `.worktrees/wip-proc-self` at 1ede96ab plus only this change.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed, the renamed `HeartbeatNote` tests among them.
- `lockup status` and `lockup spin 2` on dbg: cpu2 reported after 10 s, in the words above.

**Deliberately not done**
logz4's account of console writes still counts "ticks" an interrupts-off write cost (`log_console_waits()`); with no tick that is a period, not a lost interrupt. It is logz4's wording, left for its own change.

**How it was found**
Running `lockup spin 2` to verify idle-power step 4.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What does one beat of a CPU's heartbeat stand for now, and how often does it come?
2. Why is the unit of the threshold a check rather than a time?
3. Why can the word "tick" not describe the heartbeat on a one-shot CPU any more?
