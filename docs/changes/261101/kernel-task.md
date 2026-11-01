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
