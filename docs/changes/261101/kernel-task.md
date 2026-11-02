# kernel-task changes

## `IrqRcu`: a read guard, a pointer and `irq_rcu_synchronize()` for state read with interrupts off (2026-11-01)

**Files**
- `kernel/include/ours/task/irq_rcu.hpp`, `kernel/kernel/task/irq_rcu.cpp` — new; `kernel/kernel/task/BUILD.gn` lists the source.
- `kernel/commands/irq_rcu_test.cpp`, `kernel/commands/BUILD.gn` — new test, `irqrcu.test`.
- `.agents/tasks/irq-multi-arch/PLAN.md` — step 8 marked done.

**Symptom**
None, nothing uses it yet. The kernel had no way to free state that other CPUs read without a lock. The directory of interrupt controllers (D10 (b), D16 of `.agents/tasks/irq-multi-arch/DESIGN.md`) is read by control calls and, through a controller's slot, by the interrupt path itself, and may be removed after boot; a removed domain's table cannot be freed while a CPU that loaded its pointer is still using it.

**Cause**
A missing piece, not a mistake. The design (`DESIGN.md`, "Reclaiming a removed domain", D18) chose RCU whose readers run with interrupts off and whose grace period is one inter-processor interrupt rendezvous, because the tree has no tick to hang quiescent-state reports on, an idle CPU sleeps until an interrupt, and the scheduler is ask-first.

**Change**
Three pieces, in `ours::task`:

```cpp
class IrqRcuReadGuard;                    // interrupts off for its life; nests
auto irq_rcu_synchronize() -> void;       // returns once every read begun before the call has ended
template <typename T> class IrqRcuPointer; // publish() release, read() acquire, retract() acq_rel
```

`irq_rcu_synchronize()` takes `cpu_hotplug_lock()`, issues a full fence, and runs an empty function on every online CPU through `IpiService::sync_exec()`; then it reads the online mask again and asks any CPU that appeared meanwhile, until a round finds none new. An interrupt is not taken on a CPU that has interrupts off, so each CPU runs the function at an instant when no read is open on it: every read is wholly before that instant, done with the object, or wholly after it, and finds the retracted pointer gone. The second read of the mask and the fence are for a CPU that joins without the hotplug lock (an application processor that took longer than the boot waited for it, which `x86_bring_aps()` carries on without): the writer's store of the retract and its load of the mask need a full fence between them, or both sides could read the old value. The joining CPU's half is the locked instruction of the x86 compare-and-swap that sets its bit; a machine with weaker ordering needs that to be sequentially consistent.

The header sits in `ours/task` and the source in `kernel/kernel/task`, not in `ours/sync` as the design said, because the function needs `IpiService` and the hotplug lock, which `kernel/kernel/sync` does not depend on (the design left this open).

**Reference**
None, this project's own design (D18). The pattern is the one the sampler already uses to make sure no CPU is writing a buffer it frees: publish null, `sync_exec()`, then free (`kernel/lib/sampler/src/session.cpp`, `stop_locked()`); this step makes it a library with a reader side.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before.
- `irqrcu.test` on four CPUs, 23 checks, all ok: the pointer (nothing before publish, the object under a guard, retract once, nothing after); the guard (interrupts off, nested keeps them off until the outermost ends, one taken with interrupts off changes nothing); `synchronize()` with no reader returns in a few hundred microseconds, twice; the barrier (a reader with interrupts off on the highest CPU keeps a `synchronize()` called from the lowest from returning for 50 ms, and it returns after the reader leaves, having seen it leave); the stress (400 rounds of publish, retract, synchronize, mark dead, with two readers looking at the object for 2000 spins each: 816 sightings and none found it dead); the highest CPU taken offline, `synchronize()` returns with it away and again with it back. The other kernel-shell tests of the interrupt layers unchanged and ok: `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `interruptline.test` 63, `pinlines.test` 28, `interrupt.test` 58.
- clang-tidy on the two sources and the test: no finding.
- Four mutations, each restored byte for byte, each killed by `irqrcu.test`: the rendezvous run on this CPU only; only the boot CPU asked; `retract()` leaving the pointer; the guard leaving interrupts on. A fifth, removing the call, did not compile (the function became unused) and was replaced by the first.
- Not verified, argued only: the fence and the re-read of the online mask (a late application processor cannot be provoked here, so removing either survives); the hotplug lock (a CPU changing state during a `synchronize()`); a weak-memory machine; more than four CPUs.

**Deliberately not done**
- Nothing in `kernel/irq` uses it: the directory is step 10, and the facade step 11.
- No timeout or lockup message when a CPU stays in a read for long: the wait is as long as the longest span with interrupts off, as a TLB shootdown is, and holds the hotplug lock for as long.
- No epoch counter and no reports from the scheduler: see the design.

**How it was found**
By the design of D16 (`DESIGN.md`, "Reclaiming a removed domain", step 7), whose reviewers named the late-processor hole and the thread-context entrants.

**The user's prediction**
None. The user left the argument to the assistant ("你直接按步骤做，别让我写论证").

**Three questions you should be able to answer**
- Why can a reader that has interrupts off not be in the middle of a read when the rendezvous function runs on its CPU, and what would break if the guard only disabled preemption?
- Why does `retract()` come before `irq_rcu_synchronize()`, and what does a reader that begins after the retract find?
- Why does the function hold `cpu_hotplug_lock()`, and what is the second read of the online mask for?

## `IrqRcuReadGuard` holds preemption off too, and the test decides by the reader, after the independent review (2026-11-01)

**Files**
- `kernel/include/ours/task/irq_rcu.hpp`, `kernel/kernel/task/irq_rcu.cpp` — the guard, the asserts, the comments.
- `kernel/include/ours/cpu.hpp` — `Cpu::set_state()` joins with a sequentially consistent exchange.
- `kernel/commands/irq_rcu_test.cpp` — rewritten.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md`, `.agents/skills/ours-kernel-design/references/task.md` — the read side, the test lists, the hotplug-lock bullet.

**Symptom**
Latent, found by reading. The guard kept interrupts off and nothing else, so in a thread it left the preemption count at zero. A read that woke another thread on the same CPU (the line layer's handler path run from a thread, as the tests do it, ends in an interrupt object's trigger, which signals a waiter) set a pending reschedule, and the next time the count fell to zero, at the release of a lock that disables preemption, the reading thread was switched out with its read open. The CPU then ran another thread with interrupts on, took the rendezvous, and the writer freed what the parked read still used.

**Cause**
The argument of the previous entry said "interrupts off, so the CPU takes no rendezvous, so no read is open at that instant". That holds per CPU. A read is per thread, and a thread that is switched out with interrupts off is neither before the instant nor after it. In a handler it cannot happen, because the interrupt preamble raises the preemption count; only the guard taken in a thread was exposed. The previous entry's first question ("what would break if the guard only disabled preemption?") had the right idea and the wrong direction: it is the guard that also has to disable preemption.

**Change**
The guard is an `AutoInterruptDisabler` and, once a thread exists, an `AutoPreemptDisabler` declared after it, so the members are destroyed in the order that delivers a held-back reschedule with interrupts still off and nothing left open. Before the first thread, at boot (the line layer's first readers run at an earlier init level than threading), there is none to switch and nothing to hold; the check is whether the current thread exists. A read therefore also never wakes a thread; the header says so.

Four smaller changes from the same review:
- `Cpu::set_state()` makes the compare-exchange sequentially consistent when a CPU joins (it was release). The fence in `irq_rcu_synchronize()` is one half of a store-and-load pair with the joining CPU's store of its bit and its first read; on x86 the locked instruction gave the other half, which the design had said it did not rely on. It runs once per CPU that joins.
- The two checks that protect memory (interrupts on, preemption on) are `ASSERT`, so they hold in the release image, where a call from inside a read would return with the read open. The idle thread and the thread that already holds `cpu_hotplug_lock()` are `DEBUG_ASSERT`ed out.
- The comments now say which half carries the argument: the fence and the join's order. The lock and the second read of the mask narrow what can change during the call; they are not needed for it, which corrects the answer to the previous entry's third question.
- `publish()` asserts the pointer is empty, and both classes are `final`.

The test is rewritten. The reader decides whether the call had returned, as the last act inside its own section, instead of a timer on the command thread (which could pass by luck on a slow machine, and depended on that thread being woken while a CPU's interrupts were off). It runs once for every online CPU but the writer's. The stress uses a fresh object every round that is marked dead for good, and checks it on every spin, so a late reader finds it dead however long after the mark it looks; every reader must have seen the object, and in a quarter of the rounds in all. The offline case picks its CPU as `hotplug.test` does (not the clock owner) and, once the CPU is back, runs the barrier with the reader there, which replaces a verdict that could not fail. A guard in a thread is checked to raise the preemption count by one and give it back.

Corrections to the previous entry: it said the header sits in `ours/task` "not in `ours/sync` as the design said"; `PLAN.md` step 8 said `ours/sync`, the design did not name a directory. Its Reference said "None", but the mechanism has counterparts: in Linux, `rcu_read_lock()`, `rcu_dereference()`, `rcu_assign_pointer()` and `synchronize_rcu()`, where a span with interrupts off counts as a reader, `synchronize_rcu_expedited()` for a grace period forced by IPIs, and `kick_all_cpus_sync()` for an empty function run on every CPU (from memory, the names not re-read); Zircon has nothing of the kind in this tree. It listed "no lockup message" as not done without saying that the design's adopted note (the wait logs once past a bound) was dropped: `IpiService::sync_exec()` spins with no bound and no hook, and the lockup detector already reports a CPU stuck with interrupts off. Its count of 23 checks included one that could not fail.

**Reference**
See the corrections above for the Linux names. The wake-then-switch hazard is the one the spin-lock options already encode: `Guard<SpinLock, IrqSaveAndPreemptDisable>` "where the scope reaches `reschedule_common()` or wakes a thread" (`ours/sync/spin_lock.hpp`).

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before, with `cpu.hpp` rebuilding everything.
- `irqrcu.test` on four CPUs, 34 checks (23 before), all ok, three runs: the barrier now runs for each of CPUs 1, 2 and 3 with the writer on CPU 0 (4 checks each, the reader deciding), the stress has three readers (1189 to 1201 sightings in 400 rounds, none dead, every reader saw some), the offline case runs the barrier again once the CPU is back, and a guard in a thread raises the preemption count by one. With two CPUs, 26 checks, ok. With one CPU the boot itself stops in this environment (the tick device I8253 fails and nothing else serves it, before any command runs), so the one-CPU path of the test was not run. The other kernel-shell tests of the interrupt layers unchanged and ok: `irqdomain.test` 87, `irqchip.test` 18, `irq.test` 72, `interruptline.test` 63, `pinlines.test` 28, `interrupt.test` 58.
- The first version of the rewritten test failed on every run, for a reason in the test: the command thread, not pinned, sat on a reader's CPU, the reader held that CPU with interrupts and preemption off, and the writer the command had yet to start waited for the reader to give up after 2 s. Pinning the command thread to the writer's CPU fixed it; the old test had hidden this by luck of placement (it used one reader CPU).
- clang-tidy on `irq_rcu.cpp` and the test: no finding on a changed line; the formats of the four C++ files are clean.
- Six mutations, each restored byte for byte, each killed: the rendezvous run on this CPU only (5 of 34 checks fail); only the boot CPU asked (5); CPU 3 never asked (3); the guard not holding preemption (1, the new check of the count); `retract()` leaving the pointer and the guard leaving interrupts on (killed, the count of failed checks was not recorded). The first two of those show the barrier is not decided by timing on a slow machine; the third that every CPU is covered, not only the first reader's.
- Not verified, argued only: the fence and the second read of the mask (a late processor cannot be provoked here, so removing either survives); the sequentially consistent join in `Cpu::set_state()` (x86 already gave it, so a weaker choice survives); a handler as the reader (a task run by `sync_exec()` on the reader's CPU) and a sleep of a fixed length in place of the rendezvous; weak-memory machines; more than four CPUs; the release image's asserts firing, since nothing calls the function wrongly.

**Deliberately not done**
- The lock and the second read of the mask stay, though the argument does not need them: removing them departs from D18 as written, and they cost one mutex that sampler start and stop and the hotplug transitions already take. Dropping the lock is an option for the user.
- No test of a reader in a handler (a task run by `sync_exec()` on the reader's CPU), and none that a sleep of a fixed length instead of the rendezvous is caught: a hold longer than the sleep would catch it, and a hold of that length risks the lockup detector.

**How it was found**
By the independent review of `deceb831` (three reviewers, 2026-11-01): the correctness reviewer found the parked read and the join's ordering, the test reviewer the vacuous stress and barrier, the conventions reviewer the record's mistakes.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a read per thread and the rendezvous per CPU, and how can a thread be parked in a read with its CPU's interrupts off?
- Why is the guard's preemption disabler declared after the interrupt disabler, and what runs when it is destroyed?
- Which half of the pair does `Cpu::set_state()` supply, and what could a late processor read if it did not?
