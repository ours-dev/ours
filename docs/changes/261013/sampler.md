# sampler changes

## A sampling session follows a CPU going offline and coming back (2026-10-13)

**Files**
- `kernel/lib/sampler/include/sampler/hotplug.hpp`, `kernel/lib/sampler/BUILD.gn` — new: `begin_hotplug()`, `end_hotplug()`, `on_cpu_offline()`, `on_cpu_online()`, and the `:hotplug` interface the task layer depends on
- `kernel/lib/sampler/include/sampler/session.hpp`, `kernel/lib/sampler/src/session.cpp` — their definitions, friends of `Session`
- `kernel/kernel/task/cpu_hotplug.cpp`, `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/BUILD.gn` — the control lock held across each transition; the CPU cancels its sampling timer before its tick stops and arms it once its tick runs again
- `kernel/commands/hotplug_test.cpp` — `check_sampler()`
- `docs/repairs/261007/sampler.md` — removed: item 5 was its last

**Symptom**
`docs/repairs/261007/sampler.md` item 5. Since CPUs can go offline (2026-10-13), a running session's timer on a CPU going offline moved with the CPU's timer queue to the CPU that took it over:
- that CPU sampled twice per period;
- a session stopped while the CPU was away, then started again once it was back, hit `Timer::activate()`'s "Timer activated while still queued" assertion and panicked. The stop's IPI does not reach an offline CPU, so the moved timer was never cancelled.

**Cause**
A session arms and disarms its per-CPU timers by IPI to the CPUs online at the time (`start()`, `stop_locked()`); nothing tied a session to a CPU's going and coming.

**Change**
The objects:
- **The control lock** (`Session::LOCK`): every start and stop takes it; now the thread driving a CPU's change of state holds it across the change too.
- **The CPU's timer slot** (`TIMERS`, one per CPU, in permanent per-CPU memory).

The events:
1. `take_cpu_offline()` takes the hotplug lock, stops the Dpc workers, then takes the control lock (`begin_hotplug()`) for the transition.
2. The CPU going offline runs `on_cpu_offline()` before its tick stops: its own timer is cancelled, on its own CPU, as `disarm_this_cpu()` does in a stop. Its queue then holds no sampling timer to hand over.
3. `bring_cpu_online()` takes the control lock for the transition. The CPU coming back runs `on_cpu_online()` once its tick runs again: if a session is running, its timer is armed with the session's period, as a start would have armed it.
4. The control lock is let go after each transition (`end_hotplug()`).

Why the lock (the user's choice, i, 2026-10-13): without it a start racing an offline could arm the CPU just before it left, and a stop racing an online could miss the CPU just back. The lock order is the hotplug lock, then the control lock. Nothing takes them the other way round: a session never takes a CPU offline.

The stop's barrier holds again with this: it relies on every writer having finished once `sync_exec()` returns, which assumes every CPU that can write is in the online mask the IPI goes to. Now a CPU that has a timer is online, and an offline one has none.

Rejected: (ii) letting a moved timer notice it is on the wrong CPU and stop. It fixes the double sampling, but not the assertion: the moved timer is still queued when the CPU is back and a new start arms it.

**Reference**
Linux `perf_event_exit_cpu()` and `perf_event_init_cpu()`, perf's CPU hotplug callbacks, run under `cpus_write_lock()`, which perf's own control paths exclude with `cpus_read_lock()`. Zircon `mp_unplug_current_cpu()` shuts the sampler down first on the CPU going offline.

**Verification**
- Built in `.worktrees/wip-proc-self` at 3e1d9ea4 plus only this change, with and without `cpu_hotplug`; clang-tidy reports nothing on the changed lines.
- `boot_check.py`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- `kshell_run.py hotplug.test samplerclaim.test timer.test` on dbg: all ok. `hotplug.test`'s `check_sampler()` runs a session at the longest period (1 s): start, CPU 3 offline, stop, CPU 3 back, start again, stop. It also checks that the start-to-back stretch took less than a period, so that no timer fired in it.
- Mutation: with `sampler::on_cpu_offline()` not called, the second start panics with "Assertion failed on !timer.is_linked() with message: Timer activated while still queued", the bug item 5 described.

**Deliberately not done**
- Checking from the dump that the CPU back online records again: the check above shows it is armed, and reading a dump inside the kernel needs a user-copy writer this test does not have.
- The original item's acceptance asked for a stress test taking CPUs up and down during a run. `hotplug.test` takes one CPU down and up three times, once during a session; a loop over every secondary is left for when deep idle makes offlining a routine event.

**How it was found**
None.

**The user's prediction**
None. The user chose option i, the control lock across each transition.

**Three questions you should be able to answer**
1. Why does the stop of a session not cancel a timer that moved with an offline CPU's queue?
2. Why is the sampling timer cancelled before the tick stops, and armed only after it runs again?
3. What would a session start that raced a CPU going offline do without the control lock held across the transition?
