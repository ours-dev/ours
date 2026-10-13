# kernel-task changes

## A CPU can go offline and come back, waiting with interrupts off in between (2026-10-13)

**Files**
- `kernel/include/ours/task/cpu_hotplug.hpp`, `kernel/kernel/task/cpu_hotplug.cpp` — new: `task::take_cpu_offline()` and `task::bring_cpu_online()`, serialised by one mutex
- `kernel/include/ours/task/idle_thread.hpp`, `kernel/kernel/task/idle_thread.cpp` — `IdleThread::transition()`, `has_pending_transition()`, `state()`, and `go_offline_and_wait()`, the sequence the CPU runs on itself
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp` — `pick_next_thread()` runs the idle thread while it has a transition pending; `Scheduler::Current::deactivate()` and `reactivate()`
- `kernel/include/ours/ipi_service.hpp`, `kernel/kernel/task/ipi_service.cpp` — `close_mailbox()`, `open_mailbox()`, and `sync_exec()` answering for a CPU whose mailbox is closed
- `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp` — `TimerQueue::take_over_from()`
- `kernel/include/ours/time/tick_device.hpp`, `kernel/kernel/time/tick_device.cpp` — `stop_for_offline()`, `restart_after_offline()`
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp` — `cancel_timer()`
- `kernel/include/ours/time/clock_source.hpp` — `ClockSource::Default::cpunum()`, the jiffies CPU, which may not go offline
- `kernel/include/ours/dpc.hpp`, `kernel/kernel/task/dpc.cpp` — `DpcRunner::take_over_from()`
- `kernel/lib/arch/include/arch/tlb.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/tlb.hpp` — `arch::tlb_invalidate_all_global()`, by toggling CR4.PGE
- `kernel/commands/cpu.cpp`, `kernel/commands/hotplug_test.cpp`, `kernel/commands/dpc_test.cpp`, `kernel/commands/BUILD.gn`, `kernel/kernel/task/BUILD.gn` — `cpu status|offline|online`, `hotplug.test`, and a take-over phase in `dpc.test`

**Symptom**
None, a new capability: step 2 of `.agents/tasks/cpu-hotplug/PLAN.md`.

**Cause**
None, a new capability.

**Change**
The objects:
- **Active mask**: the scheduler's "this CPU takes threads"; cleared under the CPU's own run queue lock.
- **Online mask**: what every IPI and TLB-shootdown sender reads first.
- **IPI mailbox**: each CPU's queue of `sync_exec()` requests, now with a `closed` flag under its lock.
- **TimerQueue**: each CPU's timers; the tick is one of them in one-shot mode.
- **IdleThread state**: `{current, target}`; a pending transition makes the scheduler run the idle thread.

The events, taking CPU 3 offline from a thread on CPU 1:
1. CPU 1 stops CPU 3's Dpc workers (step 1's `stop_workers()`).
2. CPU 1 sets CPU 3's idle thread's target to Offline, sends a reschedule IPI, and waits.
3. CPU 3's scheduler picks the idle thread at once, runnable threads or not.
4. CPU 3's idle thread, interrupts off:
   - clears Active under its run queue lock, and moves every queued thread that may run on another active CPU to one;
   - leaves the online mask;
   - closes its IPI mailbox, running what was queued;
   - cancels its tick Timer and shuts its timer device down (or leaves its broadcast group);
   - records Offline, signals CPU 1, and spins with interrupts off until the target changes.
5. CPU 1 moves CPU 3's timers and queued Dpcs onto its own queues and arms its own device.

Bringing it back reverses step 4 on CPU 3: a global TLB flush first, since it took no shootdown while away, then the tick device in the mode it had, the mailbox, the online mask and Active. CPU 1 then starts its Dpc workers again.

Why each piece is where it is:
- The scheduler hook. The idle thread otherwise runs only when nothing else wants the CPU, which on a busy CPU is never. With it, the running thread is always the idle thread when the drain starts, so every other thread is queued and can be moved (Zircon `scheduler.cc` selects its idle/power thread while it has pending power work).
- Active cleared under the run queue lock. `find_active_scheduler_for()` reads Active under the target's run queue lock before placing a thread, so a placement either lands before the clear, and the drain moves it, or sees the CPU inactive and goes elsewhere.
- The closed mailbox. Online is cleared first, but a sender may have read the mask just before. Without the flag, that sender either waits for ever, or gives up and leaves its request, which lives on its stack, linked in the mailbox for the CPU to find when it returns. Closing in the same critical section that senders queue in means a request is either run at the close or never queued.
- Cancelling the tick Timer before the take-over. Otherwise it would move too, and tick CPU 1 twice per period.
- Waiting with interrupts off rather than halting. A halted x86 CPU comes back only through INIT-SIPI, and the AP start-up code is `INIT_CODE`, freed after boot. Step 3 keeps it resident and halts.

Threads pinned to the CPU follow Zircon (decision ii): one queued stays queued until the CPU is back; one woken while it is offline fails selection's "At least one CPU was assigned to us" assertion once the online bit is clear.

**Reference**
Zircon `mp_unplug_cpu_mask_single_locked()` and `mp_unplug_current_cpu()` (`zircon/kernel/kernel/mp.cc`); `IdlePowerThread::TransitionActiveToOffline()` and the pending-power-work selection in `Scheduler` (`zircon/kernel/kernel/scheduler.cc:1115`); `Scheduler::MigrateUnpinnedThreads()`; `TimerQueue::TransitionOffCpu()`; `DpcRunner::TransitionOffCpu()`; `mp_sync_exec()` treating an offline CPU as done. Differences:
- the offline CPU spins rather than halting;
- the timer device is shut down by the CPU itself;
- the mailbox is closed, where Zircon's Rust `sync_exec` notes the same hazard unsolved (`kernel/mp.rs:224`).

**Verification**
- Built in `.worktrees/wip-proc-self` at f6e87977 plus only this change; clang-tidy reports nothing on the changed lines.
- `boot_check.py`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- `kshell_run.py hotplug.test dpc.test threadjoin.test timer.test timershare.test sleep.test waitqueue.test tickbcast.test` on dbg: all ok. `hotplug.test` makes 30 checks over two round trips of CPU 3, and the timer tests after it pass on the CPU that came back.
- The same `hotplug.test`, `timer.test` and `sleep.test` on a build with `time_oneshot=false` ("tick device periodic"): ok, so both tick restart paths are exercised.
- Mutations, each failing `hotplug.test`:
  - every queued thread put back instead of moved: "every spinner goes on elsewhere";
  - `TimerQueue::take_over_from()` dropped: "the thread asleep there wakes on time, elsewhere".

**Deliberately not done**
- Halting the CPU and bringing it back with INIT-SIPI: step 3.
- A sampling session's timer on the CPU moves with the queue: `docs/repairs/261007/sampler.md` item 5 now describes what that does.
- Making the offline faster: on a busy CPU the stop waits for its Dpc workers to get the CPU behind the runnable threads, about 300 ms with two spinners per CPU (`docs/repairs/261013/kernel-task.md` item 1).
- Fair: the drain looks at each queued thread once by taking the head and moving it or putting it back at the tail, which is right for a FIFO; Fair is out of the build and the user's.

**How it was found**
The first run of `hotplug.test` failed three checks, all the test's own:
- A spinner read its CPU, was moved off it as the CPU went offline, then read the "offline" flag, and reported running on an offline CPU. The two reads are now made with interrupts off.
- The sleeper slept 50 ms, but the first offline took 330 ms, so it woke on CPU 3 before the CPU left. It now sleeps 1 s, and a separate check says the offline finished while it slept.

**The user's prediction**
None. The user chose rule ii for pinned threads; the concurrency arguments (a wakeup racing the Active clear; a Dpc racing a worker's stop) are deferred, as listed in PLAN.md.

**Three questions you should be able to answer**
1. Why must Active be cleared under the CPU's own run queue lock, and what would a wakeup on another CPU do if it were cleared without it?
2. What two failures does the closed mailbox prevent, and why is clearing the online bit first not enough?
3. Why is the tick Timer cancelled on the CPU going offline rather than moved with the other timers?
