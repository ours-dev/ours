# sampler outstanding repairs

## 5. A session's start and stop reach only the CPUs online at the time

**Severity**
Medium: latent, no CPU comes up or goes down after boot today.

**Symptom**
Latent, cannot be triggered today; it would be once CPUs can come up or go down at run time. `start()` arms a timer on `Cpu::online_mask()` and `stop_locked()` sends its barrier there. A CPU that came up during a run would sample nothing, which is harmless. One that went down during a run would keep or lose its timer depending on how it went down, and the barrier argument, that every writer has finished when `sync_exec()` returns, assumes every CPU that can be writing is in the mask the IPI goes to.

**Cause**
The control plane snapshots the online mask; nothing ties a session to CPU bring-up and tear-down.

Since 2026-10-13 a CPU can go offline (`task::take_cpu_offline()`, `.agents/tasks/cpu-hotplug`). While a session runs, the CPU's armed sampler timer moves with the rest of its timer queue to the CPU that takes it over, which then takes two samples per period until the session stops; the timer stops itself at its first expiry after that, since its handler finds the session gone. A session started after the CPU is back, before that moved timer has stopped, trips `Timer::activate()`'s "activated while still queued" assertion when it arms the CPU again.

**Change**
When CPU hotplug is added, give the session hotplug callbacks that run under the control lock: arm the timer on a CPU that comes up during a run, and cancel it on that CPU before it goes down.

**Reference**
Linux perf's CPU-hotplug callbacks, `perf_event_init_cpu()` and `perf_event_exit_cpu()`.

**Acceptance**
A stress test that takes CPUs up and down during a run finds every record intact and no timer left on a CPU that went down.
