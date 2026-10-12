# sampler outstanding repairs

## 5. A session's start and stop reach only the CPUs online at the time

**Severity**
Medium: latent, no CPU comes up or goes down after boot today.

**Symptom**
Latent, cannot be triggered today; it would be once CPUs can come up or go down at run time. `start()` arms a timer on `Cpu::online_mask()` and `stop_locked()` sends its barrier there. A CPU that came up during a run would sample nothing, which is harmless. One that went down during a run would keep or lose its timer depending on how it went down, and the barrier argument, that every writer has finished when `sync_exec()` returns, assumes every CPU that can be writing is in the mask the IPI goes to.

**Cause**
The control plane snapshots the online mask; nothing ties a session to CPU bring-up and tear-down.

**Change**
When CPU hotplug is added, give the session hotplug callbacks that run under the control lock: arm the timer on a CPU that comes up during a run, and cancel it on that CPU before it goes down.

**Reference**
Linux perf's CPU-hotplug callbacks, `perf_event_init_cpu()` and `perf_event_exit_cpu()`.

**Acceptance**
A stress test that takes CPUs up and down during a run finds every record intact and no timer left on a CPU that went down.
