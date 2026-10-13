# cpu-hotplug: a secondary CPU goes offline, halts, and comes back

**Date**: 2026-10-13

## Delivered
- f6e87977: a CPU's Dpc workers can be stopped and started again (`DpcRunner::stop_workers()`), and `dpc.test`.
- e955bb94: `task::take_cpu_offline()` and `bring_cpu_online()`, with the CPU waiting with interrupts off:
  - the idle thread's transition, and the scheduler's hook for it;
  - the run-queue drain;
  - the closed IPI mailbox;
  - the tick stopped and restarted;
  - timers and Dpcs taken over;
  - the `cpu` command and `hotplug.test`.
- 3e1d9ea4: the offline CPU halts and is held in INIT. It comes back through the AP trampoline with INIT and two SIPIs, behind the build argument `cpu_hotplug`, and boot sends the second SIPI too (`docs/repairs/260909/x86-mp.md` items 1 and 2).
- 8bc5efee: a sampling session follows a CPU's going and coming (`docs/repairs/261007/sampler.md` item 5).

## Verification
- Every step: built and booted in `.worktrees/wip-proc-self` at the step before plus only that step, clang-tidy clean on its lines, and a mutation its test catches.
- At 8bc5efee: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 144 passed.
- `hotplug.test` on dbg, 36 checks: CPU 3 offline and back twice under load, then once during a sampling session.
- `hotplug.test` also passes with `time_oneshot=false` (the periodic-tick restart path). With `cpu_hotplug=false` both images boot and it answers NotSupported.
- `dpc.test`, `threadjoin.test`, `timer.test`, `timershare.test`, `sleep.test`, `waitqueue.test`, `tickbcast.test` and `samplerclaim.test` pass after it in the same boot.
- Not measured: power, and the latency of an offline beyond the 4 to 330 ms the test logs.

## Deviations from the design
- Step 2 spun with interrupts off, a stage the design did not name, so that the drain could be tested before the CPU could be started again; step 3 replaced it with the halt.
- The IPI mailbox gained a closed state, which neither Zircon nor the design has (Zircon's Rust `sync_exec` notes the hazard as open).
- The sampler gained hotplug hooks under its control lock, after Linux's perf, where Zircon shuts the sampler down on the dying CPU.

## Found along the way
- A thread's exit queues its reap Dpc on its own CPU's Normal queue, so a CPU's Normal Dpc worker cannot be joined when it stops (step 1).
- Taking a busy CPU offline waits behind its runnable threads for its Dpc workers to run: `docs/repairs/261013/kernel-task.md` item 1, a performance item for the user.

## Not done
- `CpuStopper` stays unwired: the Zircon sequence chosen does not use it. Its place would be moving a running thread at an affinity change, with its two recorded defects (`docs/repairs/260926/kernel-task.md` item 5).
- Deep idle (`mwait` C-states) and tickless idle, which is where power is saved: a separate task.
- A userspace interface for hotplug (Zircon's `zx_system_powerctl`), and offlining the boot CPU: non-goals in DESIGN.md.
- The two concurrency arguments the user deferred: listed in PLAN.md's Decisions.
