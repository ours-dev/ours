# sampler outstanding repairs

## 4. Only a process that describes its own code can have its user frames symbolized

**Severity**
Medium: a missing capability rather than a wrong result; profiling any process other than the one running the session gives its user frames as bare addresses. The approach is a decision for the user.

**Symptom**
prof-fold resolves a user frame only through the `PROFP <koid>` line and the symbolizer markup the sampled process printed about itself (`examples/profile/profile.cpp`, `print_module()`). No other program prints them, and the vDSO is never described, so in a system-wide profile every other process's frames are addresses.

**Cause**
The module map is learned from inside the target, which has to cooperate, rather than from outside it. The kernel knows every process's mappings, but nothing hands them, with their build IDs, to a profiler.

**Change**
Options:
- (a) The profiler reads each sampled process's mappings and ELF headers from outside: an info topic listing a process's mappings, plus a read of the target's memory for the build ID note. It needs a right to the target process, and no change to the dump.
- (b) The kernel writes a mapping event into the session whenever an executable mapping is made during a run, and the profiler snapshots the existing ones at start.
- (c) Keep self-description, and have ort print it for any program on request.

Recommended: (a).

**Reference**
Linux perf learns every process's executable mappings from `PERF_RECORD_MMAP2` events, which can carry the build ID. Fuchsia's profiler reads a target's maps and ELF build IDs from outside it, through `ZX_INFO_PROCESS_MAPS` and the target's memory.

**Acceptance**
In a profile taken by one program, prof-fold names the user frames of another that printed no markup, such as the shell's.

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
