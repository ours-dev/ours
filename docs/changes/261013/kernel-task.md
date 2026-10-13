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

## What an idle CPU does is counted: timer interrupts, halts and time halted, per CPU (2026-10-13)

**Files**
- `kernel/lib/gktl/include/gktl/counter.hpp` — `Counter::get(cpu)`, one CPU's own count
- `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp` — the `timer.queue.runs` counter, bumped by `run_expired_timers()`, and `TimerQueue::num_runs(cpu)`
- `kernel/include/ours/task/idle_thread.hpp`, `kernel/kernel/task/idle_thread.cpp` — the `idle.halts` and `idle.halted_ns` counters around the halt, with preemption off across it; `IdleThread::num_halts(cpu)`, `halted_time(cpu)`; `m_cumulative_idle_time` removed, the counters doing its job
- `kernel/commands/idle.cpp`, `kernel/commands/BUILD.gn` — `idle.stats [<seconds>]` and `idle.test`

**Symptom**
Step 1 of `.agents/tasks/idle-power/PLAN.md`. Nothing counted timer interrupts, idle entries or idle time, so idle power could not be measured. The one idle figure there was, `m_cumulative_idle_time`, was read by nothing and wrong: an interrupt that ended the halt could preempt the idle thread on its way out, and the time other threads then ran was added as idle.

**Cause**
None, a measurement added; the old figure's error is above.

**Change**
The objects:
- **`timer.queue.runs`**: a per-CPU count of runs of the CPU's expired timers. Every timer interrupt runs them: its own device in one-shot mode, the tick in periodic mode, or a broadcast IPI.
- **`idle.halts` and `idle.halted_ns`**: per-CPU counts of halts in the idle loop and of the time from just before each halt to its end.
- **`idle.stats` and `idle.test`**: two snapshots of every online CPU's counters, a window apart, with the calling thread asleep between them.

The halt is bracketed with preemption off. The interrupt that ends it then cannot switch away from the idle thread before the end is read; the preemption it asked for is taken when the disabler goes, after the count.

The baseline, `idle.test` on dbg after boot, one second:

| CPU | timer interrupts/s | halts/s | halted |
|---|---|---|---|
| 0 | 100 | 100 | 99.7% |
| 1 | 99 | 99 | 99.5% |
| 2 | 99 | 0 | 0.0% |
| 3 | 99 | 0 | 0.0% |

- **An idle CPU is woken 100 times a second, by its tick and nothing else.** One timer interrupt per halt, and the halts take 99.6% of the time.
- **Two CPUs never halt.** `labour_routine()` in `kernel/top/main.cpp` starts two "gibberish" threads at boot, on every image, that spin logging "I'm Thread N" for ever. Whether they stay is a decision for the user, listed in PLAN.md.

**Reference**
Zircon keeps a per-CPU "processor idle time" around the halt for its energy model (`zircon/kernel/kernel/idle_power_thread.cc`, `Scheduler`), and `k idlestates printstats`. Linux keeps per-state usage and time in cpuidle and per-CPU idle and dyntick counts in tick-sched (`idle_calls`, `idle_sleeps`).

**Verification**
- Built in `.worktrees/wip-proc-self` at ab88d719 plus only this change; clang-tidy reports nothing new on the changed lines.
- `boot_check.py`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- `kshell_run.py idle.test hotplug.test` on dbg: both ok; the table above is `idle.test`'s output.
- Mutation: with the halted time not added, every CPU reads 0.0% halted and `idle.test` fails "on a quiet machine some CPU halts, and for at least half the window".

**Deliberately not done**
- Counting interrupts by cause (IPIs, device lines): only timer interrupts matter for the tick.
- Reaching the counters from user space: the measurement is a console command, as Zircon's `k idlestates` is.

**How it was found**
`idle.stats 5` was the first run; CPUs 2 and 3 at 0 halts led to `main.cpp`'s two threads.

**The user's prediction**
Not given. My estimate before measuring, about 100 timer interrupts a second per idle CPU, matched.

**Three questions you should be able to answer**
1. Why is preemption turned off across the halt, and what would the halted time include without it?
2. Why does counting runs of the expired timers catch every timer interrupt, in both tick modes and for a broadcast IPI?
3. Why do CPUs 2 and 3 take 99 timer interrupts a second but halt 0 times?

## Subsystems take part in a CPU's going and coming through registered hooks, not calls from the idle thread (2026-10-13)

**Files**
- `kernel/include/ours/cpu_hotplug_hook.hpp` — new: `CpuHotplugLevel` (Tick, Timers, Ipi), `CpuHotplugHook`, `CPU_HOTPLUG_HOOK()`
- `kernel/kernel.ld` — the resident `.cpu_hotplug.hooks` table in `.data.rel.ro`
- `kernel/include/ours/task/cpu_hotplug.hpp`, `kernel/kernel/task/cpu_hotplug.cpp` — `run_offline_hooks()`, `run_online_hooks()`, `cpu_hotplug_lock()`; the sampler's lock no longer taken here
- `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/BUILD.gn` — the CPU's own two steps, then the walk; no include of, or dependency on, the sampler or the lockup detector
- `kernel/kernel/task/ipi_service.cpp`, `kernel/kernel/time/tick_device.cpp`, `kernel/lib/lockup_detector/src/checker.cpp`, `kernel/lib/sampler/src/session.cpp` — each registers its own hook
- `kernel/lib/sampler/src/session.cpp`, `include/sampler/session.hpp`, `BUILD.gn` — `start()`, `stop()` and `release()` take the CPU hotplug lock before their own; `sampler/hotplug.hpp` and its `:hotplug` target removed
- `kernel/lib/lockup_detector/include/lockup_detector/lockup_detector.hpp`, `README.md`, `src/checker.cpp` — the hotplug halves private to the checker; the remaining "tick" wording of step 2 corrected

**Symptom**
The user's review of `IdleThread::go_offline_and_halt()`. It called the scheduler, the online mask, the IPI mailbox, the sampler, the lockup detector and the tick by name, one after another, and `come_back()` called them again in a reverse order kept by hand.

**Cause**
- **The dependency pointed the wrong way.** `//kernel/kernel/task`, the core, depended on `//kernel/lib/sampler` and `//kernel/lib/lockup_detector`, which themselves build on threads and the scheduler.
- **Every new per-CPU subsystem meant an edit to the idle thread.** The order between them lived only in the order of the lines.
- **The two directions were two hand-written lists**, and nothing kept one the reverse of the other.

I had argued for exactly this when the hotplug skeleton was chosen: a registry would pay off only with dozens of subsystems. I counted the subsystems and missed the direction of the dependency.

**Change**
The objects:
- **`CpuHotplugHook`**: `{level, offline, online, name}`, registered with `CPU_HOTPLUG_HOOK()` in the subsystem's own file. The linker collects them into one resident table.
- **`CpuHotplugLevel`**: the stage a hook belongs to, in the order a CPU coming back sets them up: Tick, then Timers, then Ipi.
- **`cpu_hotplug_lock()`**: the mutex a CPU's change of state holds throughout.

The events, going offline:
1. The idle thread runs the CPU's own steps: deactivate, then leave the online mask.
2. `run_offline_hooks()` walks the table from the highest level to the lowest, and within a level from the last record to the first:
   - Ipi: the mailbox closes;
   - Timers: the sampler's and the lockup detector's timers are cancelled;
   - Tick: the tick stops.

Coming back, `run_online_hooks()` walks the table the other way, then the CPU rejoins the online mask and Active. The two directions are one table walked two ways, so a level always comes down as it went up.

The table is walked once per level rather than sorted. It is read-only data, so it cannot be sorted in place as the init hooks are. With three levels and four hooks, the extra walks cost nothing.

The sampler's serialisation was turned the right way round too. `take_cpu_offline()` used to take the sessions' control lock across the transition, which made task call into the sampler. Now the sessions' `start()`, `stop()` and `release()` take `cpu_hotplug_lock()` first, then their own lock: the library builds on the core, as Linux's perf takes `cpus_read_lock()` before its own mutexes. The lock order is unchanged: the hotplug lock, then the sessions'.

The user chose this (option A, 2026-10-13) over moving the calls into `cpu_hotplug.cpp` (B) or leaving them (C, which is Zircon's own `mp_unplug_current_cpu()`).

**Reference**
- A static form of Linux's cpuhp states (`include/linux/cpuhotplug.h`, `cpuhp_setup_state()`): a table of named stages, set up in order and torn down in reverse, but without the state machine, the rollback or the per-CPU hotplug thread.
- The registration follows this tree's own `GKTL_INIT_HOOK`.

**Verification**
- Built in `.worktrees/wip-proc-self` at c61f59a5 plus only this change; clang-tidy reports nothing new on the changed lines.
- The kernel's symbol table holds the four hooks, `LockupHeartbeat`, `IpiMailbox`, `Tick` and `SamplerTimers`, between `__cpu_hotplug_hooks_start` and `__cpu_hotplug_hooks_end`.
- `boot_check.py`: dbg and rel PASS.
- `kshell_run.py hotplug.test idle.test dpc.test "lockup status" "lockup spin 2"` on dbg: all ok, `hotplug.test`'s sampler check included.
- Mutation: with the Tick hook not registered, `hotplug.test` ends the kernel in a panic.

**Deliberately not done**
- Making the scheduler's deactivation and the online mask hooks too: they are what going offline is, and their order around the hooks is the sequence itself.
- A failure path: a hook cannot refuse; one that fails logs, as the tick's restart does.

**How it was found**
The user's review, 2026-10-13: "How do you really think this is a good way???"

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did the old calls make the dependency between task and the sampler point the wrong way, and how does the hotplug lock fix the sampler's half of it?
2. Why is walking one table in two directions enough to keep going offline the reverse of coming back, levels and all?
3. Why does the walk go level by level instead of sorting the table once?

## A CPU going offline finds the threads that may leave in its run queue, and leaves the others untouched (2026-10-13)

**Files**
- `kernel/include/ours/task/sched/rr_run_queue.hpp` — `RrRunQueue::find_first(pred)`
- `kernel/kernel/task/sched/scheduler.cpp`, `kernel/include/ours/task/sched/scheduler.hpp` — `Scheduler::Current::deactivate()` picks movable threads out of the queue instead of rotating its head

**Symptom**
The user's review of `deactivate()`, which:
1. took the queue's head through `ISchedule::evaluate_next_thread()`;
2. dequeued it;
3. only then asked whether it may run elsewhere;
4. and if not, enqueued it again.

A thread pinned to the CPU was dequeued, marked Migrating and queued again for nothing.

**Cause**
The generic scheduler went through the only view `ISchedule` offers, the queue's head. To get past a pinned head it had to move it to the tail, which also made the loop depend on the queue being a FIFO: Fair would put the thread back by its vruntime, possibly at the head again. But the scheduler owns the run queue and holds the discipline's queue by its type (`RunQueue::queue<Discipline::Normal>()`, `NormalRunQueue`); it can look in it directly (the user's point).

**Change**
- **`RrRunQueue::find_first(pred)`** returns the first context from the head for which `pred` holds.
- **`deactivate()`** asks it, each round under the run queue lock, for the first thread whose affinity names an active CPU, and moves that one through `deactivate_thread()`, the discipline's dequeue, which keeps the counts. It stops when none is left.
- **A pinned thread is never touched.**
- **Each round looks afresh from the head.** The lock is let go for each move, and the queue may change meanwhile (a kill takes a thread out), so no position in it is kept. Nothing joins the queue, since Active is clear, so each round moves one thread and the loop ends.

The cost falls on Fair's return: the generic scheduler now calls `find_first()` on the `NormalRunQueue`, so `FairRunQueue` will need it too. That is the user's to add.

**Reference**
Zircon `Scheduler::MigrateUnpinnedThreads()` walks its own run queue and moves the threads whose affinity allows another CPU, leaving the pinned ones where they are.

**Verification**
- Built in `.worktrees/wip-proc-self` at 36e8e889 plus only this change; clang-tidy reports nothing new on the changed lines.
- `boot_check.py`: dbg and rel PASS.
- `kshell_run.py hotplug.test threadjoin.test` on dbg: both ok.
- Mutation: with no thread ever found movable, `hotplug.test` fails "and every spinner goes on elsewhere" in both rounds.
- `idle.test` failed in the same run, from a flaw of its own, which the next entry fixes.

**Deliberately not done**
An iteration entry in `ISchedule`: the queue's type is known at compile time, so a method on it is enough, and the discipline's virtual interface stays as it was.

**How it was found**
The user's review, 2026-10-13: "每个调度器不都能看到有哪些RunQueue吗？直接从里面拿就行了".

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did the old loop have to dequeue a pinned thread, and why did that make it a FIFO-only loop?
2. Why does each round look from the head again instead of keeping its place?
3. Which part of a move must still go through the discipline, and why?
