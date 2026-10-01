# kernel-task changes

## Title: sync_exec() runs its caller's own share with interrupts off, on the CPU it left out (2026-10-02)

**Files**
- `kernel/kernel/task/ipi_service.cpp` — `IpiService::sync_exec()` disables interrupts before reading `Cpu::id()` and restores them once the calling CPU's own share of the task has run, before it waits for the others.
- `kernel/include/ours/ipi_service.hpp` — the contract says so: the caller's share runs with interrupts off, as the others do, and the caller need not disable preemption.
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace::invalidate()` drops the `AutoPreemptDisabler` it held around the call as a workaround.
- `docs/repairs/260926/kernel-task.md` — item 1, this bug, removed. The others keep their numbers: they are older entries in the earlier format, and renumbering them would make them changed lines the document check holds to the new one. `docs/repairs/261002/kernel-task.md`, a duplicate of it written with the shootdown, removed.

**Symptom**
Latent; not reproduced on demand, because it needs the caller to be preempted inside a window of a few instructions. Two ways it goes wrong:
- A thread that reads `self = A`, is preempted and moved to B, then runs "its own share" on B: B runs the task twice (once from the IPI, once here) and A never runs it. For the TLB shootdown A keeps the stale translation; for the profiler A's counter is never programmed.
- The header promises the task "runs in interrupt context", and on every other CPU it does, inside the IPI handler. The caller's own share ran with interrupts on: a task that takes a lock an interrupt handler also takes would deadlock against the first interrupt to land on the calling CPU.

**Cause**
`sync_exec()` computes which CPU it is on, takes that CPU out of the targets, and later runs the task itself for it, all with interrupts and preemption as the caller left them. Nothing in its contract asked the caller to pin itself. The bug was recorded on 2026-09-26 (item 1 of `docs/repairs/260926/kernel-task.md`, from the per-CPU access audit), with Zircon's fix; it mattered little while the profiler was the only caller and became a correctness question when the TLB shootdown started using it. The shootdown worked around it with an `AutoPreemptDisabler` of its own.

The user chose between two fixes on 2026-10-02. Disabling preemption pins the thread, which fixes the first problem and not the second. Disabling interrupts, as Zircon does, fixes both: with interrupts off nothing can preempt the thread (preemption here happens on the way out of an interrupt or at an explicit reschedule, and the window has neither), and the local share runs in the same context as everyone else's. Its cost is that this CPU takes no interrupt for as long as the task runs, which the other CPUs already pay in their handlers. The user chose interrupts off.

Interrupts go back on before the wait, which must be able to take other CPUs' requests: two CPUs that each wait on the other with interrupts off would wait for ever. That is also why `sync_exec()` still asserts that its caller has interrupts on.

**Change**
```cpp
::arch::AutoInterruptDisabler no_interrupts;
auto const self = Cpu::id();
// ... take self out of the targets, queue the request, send the IPIs ...
if (run_here) {
    task(context);
}
no_interrupts.reenable();
while (call.outstanding.load(std::memory_order_acquire).any()) {
    arch::spin_hint();
}
```

**Reference**
Zircon `mp_sync_exec()` (`zircon/kernel/kernel/mp.cc:173`): `arch_interrupt_save()` before `arch_curr_cpu_num()`, the local task, `arch_interrupt_restore()` before the wait. Linux `smp_call_function_many_cond()`: the caller holds preemption off, and the local share runs under `local_irq_save()`.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped).
- `vmosnapshot.test` over COM1: 200 checks ok on both images, including the two-CPU shootdown check, which is now the one caller that reaches `sync_exec()` (the PMU profiler answers `NotSupported` under TCG); `usercopy.test` 18 on dbg.
- Two independent reviews of the first version of this change (preemption off rather than interrupts off) found no correctness or liveness problem: no migration is possible inside the window, the early return and the nested count from the page-fault preamble are fine, and no lock or wait in the window can deadlock. Interrupts off is strictly stronger over the same window.
- Not verified: the migration itself, which cannot be provoked on demand.

**Deliberately not done**
- The profiler's own races (`prof::Sampler::start()` publishes Running only after `sync_exec()` returns, and the tick sampler faults on `Thread::Current::tid()`, item 4 of `docs/repairs/260924/kernel-task.md`): parked by the user on 2026-10-02.
- Zircon's wait also services its own mailbox when the caller had interrupts off. Here a caller with interrupts off is refused by an assertion instead.

**How it was found**
Recorded by the 2026-09-26 per-CPU access audit; found again while writing the TLB shootdown, whose correctness depends on the CPU that is "this one" being the CPU it read.

**The user's prediction**
None; the user chose interrupts off over preemption off once the second problem was laid out.

**Three questions you should be able to answer**
- What goes wrong if the caller migrates between reading its CPU number and running its own share?
- Why does disabling preemption fix that and not the lock problem, and why does disabling interrupts fix both?
- Why must interrupts be back on before the wait?
