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

## An offline CPU halts and comes back through the AP trampoline, when the kernel is built with cpu_hotplug (2026-10-13)

**Files**
- `kernel/kernel_params.gni`, `kernel/products/BUILD.gn` — the build argument `cpu_hotplug` (default true), defining `CONFIG_CPU_HOTPLUG`
- `kernel/include/ours/init.hpp` — `CPU_INIT_CODE` and `CPU_INIT_DATA`: init sections without hotplug, kept with it
- `kernel/include/ours/task/idle_thread.hpp`, `kernel/kernel/task/idle_thread.cpp` — `go_offline_and_halt()` replaces the spin; `come_back()`; `has_halted()`; `transition(Running)` starts the CPU (`arch_activate_cpu()`)
- `kernel/include/ours/task/cpu_hotplug.hpp`, `kernel/kernel/task/cpu_hotplug.cpp` — NotSupported without hotplug; the halted CPU held in INIT before its timers are taken; a one-second bound on coming back
- `kernel/lib/arch/include/arch/tlb.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/tlb.hpp` — `tlb_invalidate_all_global()` removed: INIT flushes the TLB
- `kernel/commands/hotplug_test.cpp` — "halted" checked; NotSupported without hotplug
- The x86 side is in `docs/changes/261013/x86-mp.md`

**Symptom**
An offline CPU spun with interrupts off (step 2), which costs more power than idling.

**Cause**
A halted x86 CPU comes back only through INIT and startup IPIs, through the AP trampoline and its per-CPU start-up code, which was `INIT_CODE` and freed after boot, so step 2 could not halt.

**Change**
The objects:
- **`CPU_INIT_CODE`**: an init section, or plain text when `CONFIG_CPU_HOTPLUG` is set.
- **`m_halted`**: set by the CPU as the last write before it halts.
- **`come_back()`**: the idle thread's entry for a CPU that starts again.

The events:
1. Going offline ends in `arch_cpu_halt_offline(m_halted)` instead of a spin: `wbinvd`, the flag, then `cli; hlt` for good.
2. The control thread waits for the flag, then INITs the CPU (`arch_deactivate_cpu()`), then takes over its timers and Dpcs.
3. Coming back: `transition(Running)` calls `arch_activate_cpu()`: INIT, 10 ms, two startup IPIs.
4. The CPU runs the trampoline onto its idle thread's stack, from the top, and `x86_start_nonboot_cpu()`, which sees its idle thread Offline. It sets its hardware up again, but not the timer device it enrolled at boot, and calls `IdleThread::come_back()`.
5. `come_back()` restarts the tick, opens the mailbox, rejoins the online and active masks, signals, turns interrupts on, and enters the idle loop.

The flag is the last write for a reason. The INIT resets the CPU wherever it is; a CPU reset while it held a lock, in the signal or the log before the halt, would hold it for ever.

The configuration is the user's call (2026-10-13): with `cpu_hotplug` the start-up code stays, without it it is freed with the init sections and taking a CPU offline answers NotSupported. In the built kernel `x86_start_nonboot_cpu` and `setup_local_apic_percpu` are in `.text` with it and `.init.text` without.

**Reference**
Zircon:
- `IdlePowerThread::FlushAndHalt()` and `arch_flush_state_and_halt()` (`zircon/kernel/arch/x86/mp.cc`): `wbinvd`, signal, `cli; hlt`;
- `platform_mp_cpu_unplug()`: INIT from the control CPU;
- `mp_hotplug_cpu_mask()` → `x86_bringup_aps()`: INIT, 10 ms, two STARTUPs;
- `thread_secondary_cpu_entry()` → `ReviveIdlePowerThread()`.

Linux keeps the hotplug start-up path out of `__init` the same way, under `CONFIG_HOTPLUG_CPU`.

**Verification**
- Built in `.worktrees/wip-proc-self` at e955bb94 plus only this change; clang-tidy reports nothing on the changed lines.
- `boot_check.py`, default build: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- `kshell_run.py hotplug.test dpc.test timer.test sleep.test threadjoin.test`: all ok. CPU 3 halts and is back about 150 ms after it is started again, twice.
- A build with `cpu_hotplug=false`: both images PASS, and `hotplug.test` answers "both answer NotSupported".
- Mutation: with `restore_local_apic_percpu()` skipped on the way back, CPU 3 comes back deaf to IPIs and its timer, and `hotplug.test` hangs at the pinned thread's check (FAIL at the 60 s command timeout).

**Deliberately not done**
- A deeper halt (`mwait` in the deepest C-state, Linux's `mwait_play_dead()`): with the deep-idle work.
- Starting a CPU that was never brought up at boot: only a CPU that went offline is started again.
- The second SIPI is not observable under QEMU, which takes the first.

**How it was found**
None.

**The user's prediction**
None. The user chose to keep the start-up code under a configuration switch.

**Three questions you should be able to answer**
1. Why must `m_halted` be the CPU's last write, and what would an INIT that arrived during `m_transitioned.signal()` leave behind?
2. Why does a CPU coming back set up its local APIC again but not enroll its timer device again?
3. What does the CPU coming back run on, and why may it start from the top of its idle thread's stack?

## A per-CPU accessor that adds nothing is an alias, not an empty derived class (2026-10-13)

**Files**
- `kernel/kernel/task/ipi_service.cpp` — `using CurrentMailbox = mm::CpuLocal::Current<Mailbox>;`
- `kernel/arch/x86/kernel/io_bitmap.cpp` — `using CurrentLoaded = mm::CpuLocal::Current<Loaded>;`
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the rule, under Gotchas

**Symptom**
None; a style fix the user asked for: `class CurrentMailbox: public mm::CpuLocal::Current<Mailbox> {};` declared a new type to say nothing new.

**Cause**
An empty derived class is a second, distinct type for what is the same thing. It costs a reader the question "what does it add?", and the answer is nothing. `CpuLocal::Current<T>` keeps its per-CPU instance (`CURRENT_INSTANCE`) in the template, keyed by `T` alone, so the derived class added neither members nor storage.

**Change**
The two empty derivations in the tree became aliases, as `DpcRunner::Current` and `cpu_stopper.cpp`'s `Current` already were. Classes that do add members (`Scheduler::Current`, `IdleThread::Current`, `TickDevice::Current`, `Thread::Current`, `PmNode::Current`) stay derived. The rule is in the C++ style skill: derive only to add members.

**Reference**
None, this project's own style.

**Verification**
Built and booted with `boot_check.py`: dbg and rel PASS. `hotplug.test`, which closes and opens IPI mailboxes, passes on dbg.

**Deliberately not done**
None.

**How it was found**
The user's review, 2026-10-13.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does an alias of `CpuLocal::Current<Mailbox>` reach the same per-CPU storage as the derived class did?
2. When is deriving from `CpuLocal::Current<T>` the right choice?
3. What does a reader have to check when they meet an empty derived class that they do not have to check for an alias?
