# kernel-time changes

## TickSched loses the two fields of a tick that is stopped and restarted (2026-10-14)

**Files**
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp` — `m_last_tick` and `m_next_tick` go

**Symptom**
None. The two fields were initialised to zero and never read or written again, from the first commit that built the kernel (25c50029) on; idle-power step 4 marked them `[[maybe_unused]]` when removing `friend class TickDevice` let the compiler see it.

**Cause**
They are Linux's `tick_sched` state for NOHZ idle: `last_tick` keeps the last tick's expiry so a tick restarted after an idle stretch lands on the old period, and `next_tick` caches the next one. That needs a tick that is stopped while idle and started again. Here there is none to stop: a one-shot CPU has no tick (decision ii, Zircon's model), and a periodic device cannot be stopped while idle, because stopping a tick means arming the device for the next real deadline, a one-shot operation; Linux too keeps the periodic tick running where `tick_switch_to_oneshot()` fails. The user agreed to delete them on 2026-10-14 ("那听你的"), after asking why, and that periodic mode stays.

**Change**
The two fields and their initialisers are deleted. `TickSched` is now 8 bytes, which before 93e4b037 sent `TickSched::get()` down `CpuLocal::Current`'s by-value path and failed to compile; with that fixed it is reached by reference like any object.

**Reference**
Linux `struct tick_sched` (`kernel/time/tick-sched.h`), whose `last_tick` and `next_tick` serve `tick_nohz_stop_tick()` and `tick_nohz_restart()`.

**Verification**
- Built in `.worktrees/wip-proc-self` at 93e4b037 plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed.
- `idle.test`, `preempt.test`, `hotplug.test` on dbg, `idle.test` and `preempt.test` on the periodic build: ok.

**Deliberately not done**
None.

**How it was found**
Idle-power step 4, which removed the friend declaration that had hidden them.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can a periodic-only device not stop its tick while idle?
2. What were `last_tick` and `next_tick` for in Linux?
3. Why did deleting two unused fields first fail to compile?
