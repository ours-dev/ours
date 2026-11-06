# periodic-balance: a periodic balance of busy CPUs against each other

**Date**: 2026-11-06
**Status**: Proposed

## Problem
`Scheduler::steal_work_from()` runs only when a CPU has nothing to run (the idle loop), so a split among CPUs that all have work stays as it is (repair item 22 of `docs/repairs/261103/kernel-task-sched.md`). Four CPUs holding 5, 1, 1 and 1 threads are all busy, so nobody is idle and nobody looks: the split is 5/1/1/1 for as long as the threads run, where an even one is 2/2/2/2. The function became callable on any CPU's scheduler on 2026-11-06, and a thief that is running something already takes only what fits the mean; what is missing is something that calls it periodically. The user asked for it on 2026-11-06 and chose a global worker thread (form A below). A review of this document the same day found that a worker thread distorts the very mean it balances; the form is open again.

Repair item 23 is the same gap seen from the idle side: a halted CPU that found nothing looks again only when an interrupt wakes it. A periodic visit covers it as a side effect.

## Goals and non-goals
Goals: a periodic pass that calls `steal_work_from()` for the online CPUs; off by default; a knob set like the others (boot option, `steal.policy`); no work and no wake-ups when it is off; a test that shows a busy split evened out and the off setting leaving it alone.

Non-goals: a better load metric (the weight sum stays); an interval per scheduling domain or one that adapts to how balanced the machine is; pushing threads from a busy CPU; a balance when a thread wakes (placement is `select_target_cpu()`'s); the hotplug-return scenario as a goal of its own.

## Design
**Common to both forms.**
- The knob `balance-period-ms` (`sched_domain::Knob::BalancePeriodMs`): 0 is off, the default; up to 10000. Boot option `kernel.sched.steal.balance-period-ms`, listed and set by `steal.policy`. The sixth entry of every table that pins the five knobs today: `NUM_KNOBS`, `knob_info`, `BootOptions::STEAL_OPTIONS`, `NUM_STEAL_OPTIONS` and the initializer of `m_steal`, the ktests and host tests that pin them, and the list of knobs `steal.test` pins to their defaults for its length.
- A pass: for each CPU it covers, `Scheduler::Current::get(cpu).steal_work_from(Cpu::online_mask())`. The function holds preemption off for its own length, takes the victim's lock and then the thief's, never both, and answers 0 for a CPU that is not active, so a pass needs no hotplug code of its own. Off means no work and no wake-up: nothing is armed and no thread waits on a timer.
- The mean rule is the existing one. A thief that is running something takes a thread only if it fits what the mean leaves it (`PullBudget`, `first_is_free` false); one that is idle takes the first whatever it weighs.

**Why the worker's own weight matters** (the finding of the review). A thread that is running counts in its CPU's `load()`. While a worker thread makes its pass it is running, on the CPU `select_target_cpu()` gave it, which is the one with the fewest runnable threads (the first of them): the CPU that most needs a pull. With 5/1/1/1 default-weight spinners the worker lands on CPU 1; the loads are 5, 2, 1, 1, the mean is 2.25 and not 2, and CPU 1's deficit is 0.25 of a thread. A thief takes a thread only if the whole thread fits, so CPU 1 takes nothing. CPUs 2 and 3 each have a deficit of 1.25 and take one: the loads are 3, 2, 2, 2, which is spinners 3/1/2/2. The next pass puts the worker on CPU 1 again (the only one with one runnable thread), every deficit is at most 0.25, nothing is taken, and 3/1/2/2 is a fixed point with a spread of 2. The same state hides repair item 22's 2/3/2/1. A worker of the lowest priority (about a twentieth of a thread) leaves CPU 1 a deficit of 0.96, which is still short of a whole thread, and a higher priority makes it worse. The loads are exact multiples of a thread's weight, so a strict "must fit" sits on a knife edge with anything that perturbs the mean by a hair.

**Form A: one global worker thread** (the user's choice of 2026-11-06).
- `BalanceWorker`: a detached kernel thread with no hard affinity, started once the other CPUs are up (`gktl::InitLevel::Platform`, as the DPC workers of the secondaries are). It waits on an event with a deadline: with the period at 0 it waits without one, and `StealPolicy::set()` of the knob signals the event, so a setting made during a sleep is taken at once and an off worker causes no timer interrupts (`idle.test` allows a few per CPU in a second). A pass visits every online CPU, starting at a rotating index.
- To keep its own weight out of the mean the pass must discount it: `steal_work_from()` takes the caller's weight off the load of the CPU the caller runs on, for a caller that says it is a balancer (an argument), or the worker is given no weight in the run queue the way the idle thread has none. Either is a change inside the scheduler's accounting or its interface, and with it a worker that is ready to run but late (round-robin never preempts a thread of the same discipline for a wake-up, so on saturated CPUs a pass waits for a slice of 64 ms).
- The overlap (repair item 36). The worker's call for CPU B and B's own idle thread can run at once, and both read B's load before either has queued anything. So a flag per scheduler, a try-lock: `exchange(true, acquire)` after the enabled/active check and `store(false, release)` at every exit, declared after the preemption disabler so that it is released before a preemption point (otherwise a worker switched out in the destructor holds B's flag for slices). The invariant it protects: at most one take is in flight for a thief, from the read of its load to the moment the threads are on its queue. A second caller answers 0; if it was the idle thread, the first caller's take wakes the CPU with a reschedule request. A counter (`schedule.steal_busy`) says how often the flag was found taken, so that a test can see it work.
- A race that is older than this: a thread woken by `Scheduler::unpark_thread()` is placed by `select_target_cpu()` and queued by `do_unpark_thread()` with no re-check that the CPU is still Active, so a wake that lands as a CPU goes offline can leave the thread on a drained queue (repair item 38). For most threads another wake or the CPU's return fixes it; for the one thread that is the whole feature it would stop all balancing until the CPU came back. The worker re-arms through the same path, so it is exposed.

**Form B: a timer on each CPU** (the pattern of the lockup detector's heartbeat, `kernel/lib/lockup_detector/src/checker.cpp`).
- Each CPU has a periodic `time::Timer` (`CpuLocal`), armed on its own CPU by `IpiService::sync_exec()` on all online CPUs, whose handler runs in that CPU's timer interrupt and calls `steal_work_from()` for its own CPU, then `forward(period)` and `Restart`; with the period 0 it does not restart. A hotplug hook at `CpuHotplugLevel::Timers` cancels it as the CPU goes offline and arms it as the CPU returns, if the knob is non-zero. `StealPolicy::set()` of the knob from 0 arms the timers that are not queued; a change between two non-zero periods is taken at the next fire.
- No thread, so nothing is added to any CPU's load: the pull runs in the context of the thread it interrupted, whose weight is already counted, and an idle CPU is interrupted from its idle thread, so the first thread is free. The mean is the real mean and the timeline below holds.
- One caller per thief: a CPU's handler is the only periodic caller of its scheduler, and the idle thread calls with interrupts off, so the handler cannot meet it. Repair item 36 does not arise for these callers (it stays open for a remote caller). The timer wakes a halted CPU every period, so a halted CPU looks again (item 23). The race of item 38 does not touch a timer.
- The cost: a pull in an interrupt handler with interrupts off, for the length of one look (the idle path is the same length, also with interrupts off); the timers to arm, move and cancel with the CPUs; the global view ("CPU A balances the system") is not what this does, though the remote call stays there for a balancer that wants it.

**Timeline of a pass in form B** (4 CPUs, all busy, 5/1/1/1, mean 2, period 20 ms; each CPU's timer fires within a period of the others, in any order):
```text
CPU 1 fires   here 1 < mean 2; CPU 0 holds 5 (>= min-runnable 2): pull = min(5 - 2, 2 - 1) = 1;
              CPU 1 is running a thread, so the thread must fit: takes 1                      -> 4/2/1/1
CPU 2 fires   pull = min(4 - 2, 2 - 1) = 1: takes 1                                           -> 3/2/2/1
CPU 3 fires   pull = min(3 - 2, 2 - 1) = 1: takes 1                                           -> 2/2/2/2
CPU 0 fires   here 2, not below the mean: nothing
next period   every CPU is at the mean: nothing
```
The same timeline with form A needs the discount above, or it ends 3/1/2/2.

**Counterparts.** Linux: `sched_balance_trigger()` raises `SCHED_SOFTIRQ` from each CPU's tick; `sched_balance_domains()` walks the domains of the CPU whose interval has passed; `should_we_balance()` lets one CPU of a group do it; `sched_balance_rq(this_cpu, this_rq, ...)` pulls for `this_rq`, which is the shape `steal_work_from()` has now. Linux balances per CPU, from that CPU's own tick: form B's shape. Zircon, in `kernel/kernel/scheduler.cc` of the local Fuchsia checkout, balances when a thread is placed (`Scheduler::FindTargetCpu()`) and when a CPU goes idle (`Scheduler::StealWork()`); a search of the file for "periodic" and "rebalance" finds nothing, so this part has no Zircon counterpart. Linux also lets a task be taken when half of its load fits the imbalance (`detach_tasks()`), which is the slack the strict "must fit" of `PullBudget` lacks; it is a separate question (below).

## Alternatives considered
- **A DPC per CPU**, queued from a timer: the machinery of form B and a thread besides, with the thread's weight.
- **One worker thread per CPU:** N threads that sleep to do microseconds of work, each with the weight problem.
- **Instead of the overlap flag**, re-read the thief's load under its lock before queuing and give the excess back to the victim (a third lock step, and the threads Migrating for longer); or leave the rule with the caller. Rejected: the flag is a few lines and needs no undo. Not needed at all in form B.
- **Making a pass skip idle CPUs**, which have their idle thread: the idle thread looks once and then halts (item 23); a visit is what looks again.
- **A looser fit for a busy thief** (take a thread when half of it fits, as Linux's `detach_tasks()`): it would let CPU 1 above take its thread with a deficit of 0.96 but not with 0.25, so it does not fix form A by itself, and it changes what every busy look does. Kept as a question.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Form of the trigger | A, a global worker thread (with the discount and the flag); B, a timer on each CPU | B: it has no thread, so the mean is the real mean, it needs no flag or discount, it cannot be stranded, it costs nothing when off, and the hook pattern exists in the lockup detector. A keeps the global view the user pictured | None (predictions are paused) | A on 2026-11-06, before the finding; reopened |
| Default of the period | off (0); a value | off, so that nothing that exists changes until it is measured | None | |
| Whether idle CPUs are visited | yes; no | yes: the visit is what looks again for a halted CPU | None | |
| Whether a busy thief may take a thread that half fits | no (strict, as now); yes (Linux's slack) | no, until a measurement asks for it | None | |
| The overlap, if form A | a flag per thief; re-read and give back; the caller's rule | the flag | None | |

## Testing and verification
- Host: the knob table pinned as literals (`kernel/lib/sched_domain/test`, which already pins the five).
- Kernel (`ktest`): `StealPolicy.*` and `BootOptions.TheStealOptionsAreReadByIndex` take the sixth knob, and an unset sixth option reads as none.
- `steal.test`: a balance round with no CPU idle (every online CPU holds a pinned thread, the victim holds more, then they are let go to every CPU), the period set, and the round waits on the pass counter and not on a clock. The criterion is that the spread of thread counts falls to at most half of what it was, on the lanes whose CPU count makes the mean a whole number of threads; with more CPUs in view a busy thief's deficit is under a thread and the passes settle short of even, which `steal.test` already notes for the idle rounds. The negative control is the period at 0: the split is where it was. The pin list gains the sixth knob at 0.
- Form A only: a round that calls the same thief from two CPUs at once, with a barrier, and checks `schedule.steal_busy` rose and the pair took no more than one call would; a host test of the flag proves mutual exclusion but not the orderings, which would need a plain variable under the flag and a weakly ordered machine or a sanitizer.
- `verify`: `boot_check.py` on the three lanes, `ktest all`, the 12 console tests (`idle.test` with the setting off must be unchanged), as for every step of this series.
- Cannot be verified here: the cost of a pass on a machine with many CPUs (a pass is linear in them; QEMU has four or eight), and form A's late passes under a load that never lets the worker run.

## Open questions
- Whether the strict fit is right for a busy thief, or the half-thread slack of Linux, once passes run: measure what the settled splits look like.
- What the default period should be once the measurement of `steal-policy-measure` has the setting in its sweep.
- Whether a pass should stop early when nothing was taken for some passes, to stop waking CPUs: not needed on this machine.
