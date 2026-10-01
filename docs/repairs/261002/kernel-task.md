# kernel-task outstanding repairs

## 1. sync_exec() reads which CPU it is on with preemption enabled

**Severity**
Medium: wrong on a path a console command reaches today (`prof.start`), when the caller is preempted at the wrong moment.

**Symptom**
Latent. A caller of `IpiService::sync_exec()` that is preempted and migrated between `Cpu::id()` and its own share of the work runs `task` on the CPU it moved to, and the CPU it started on, removed from the targets as "this one", never runs it. For `prof.start` that CPU's counter is never programmed; for a TLB shootdown it would keep a stale translation.

**Cause**
`sync_exec()` computes `self = Cpu::id()` and `run_here`, removes `self` from the targets, and later runs `task(context)` itself, all with preemption as the caller left it. Nothing in its contract asks the caller to disable preemption. `ArchVmAspace::invalidate()` disables it around the call for this reason; `prof::arch::start_pmu_sampling()` and `stop_pmu_sampling()` do not.

**Change**
Hold a `task::AutoPreemptDisabler` inside `sync_exec()` from `Cpu::id()` to the end, so that every caller is safe without knowing it (Zircon's `mp_sync_exec()` pins it another way: it disables interrupts with `arch_interrupt_save()` before reading `arch_curr_cpu_num()`, runs its own share, and restores them only before it waits for the others). Then drop the one in `ArchVmAspace::invalidate()` or leave it as documentation.

**Reference**
Zircon `mp_sync_exec()` (`zircon/kernel/kernel/mp.cc:173`); Linux `smp_call_function_many()`, which requires preemption disabled and warns otherwise.

**Acceptance**
`sync_exec()` holds preemption off across its use of `self`; `prof.start` and `vmosnapshot.test` still pass on both images.
