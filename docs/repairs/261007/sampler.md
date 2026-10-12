# sampler outstanding repairs

## 2. A released session gives up its claim before its memory, so a stalled read lets a second session's buffers coexist with the first's

**Severity**
Medium: only a holder of the SAMPLING resource can reach it, and only while a read's copy to user memory is stalled.

**Symptom**
Latent, cannot be triggered by today's programs. Closing a session's last handle runs `SamplerDispatcher::on_zero_handles()`, then `Session::release()`, which clears `CLAIMED` at once. A `sampler_read` in flight holds an `Arc<SamplerDispatcher>`, so the session and its buffers (up to `SAMPLER_MAX_BUFFER_SIZE` per possible CPU) are freed only when that copy returns. Meanwhile a new `sampler_create` succeeds and allocates a full set of its own; repeating create, start, stop, a read that stalls, close stacks one set per round.

**Cause**
`Session::create()` claims before it allocates so that creates cannot allocate side by side and run the kernel out of frames, where the allocator retries instead of failing (`docs/repairs/260908/heap.md` item 3). That bound holds only while the claim is held until the memory is gone. `release()` gives the claim back when the last handle goes, which is earlier whenever a reader still holds a reference.

**Change**
Give the claim back when the memory goes: a claim token as the session's first member, so it is destroyed after the slots and the unwind index, and `release()` keeps only the stop. Ruled out: making `release()` wait for readers, which would block a handle close on a user copy.

**Reference**
None, this project's own design.

**Acceptance**
By argument, since the stall cannot be forced today: `CLAIMED` is cleared only after the session's slots are freed, so a create between release and destruction returns AlreadyExists. The shell's create, close, create-again check still passes, and both images boot.

## 3. User ticks that are never recorded are not counted, and a user record's time and CPU are the walk's, not the tick's

**Severity**
Medium: the dump's tick count falls short whenever a run ends with threads still marked, and a user record's time is late whenever its thread is preempted between the tick and its return to user space.

**Symptom**
`note_unmatched()` exists so that the dump accounts for every tick (`Session::on_sample_due()`), but a tick that marks a user thread can still come to nothing without a trace: the thread is killed first (`process_pending_signals()` handles Kill before SampleStack), the run ends before the walk (`sample_user_stack()` finds ACTIVE cleared), or another run has begun (the generation differs). And `sample_user_stack()` takes `current_context()` when it records, so the timestamp and CPU are those of the walk; a thread preempted or migrated on its way out carries a later time and possibly another CPU's number.

**Cause**
`StackSampleRequest` carries the pc, the frame pointer and the generation, but not the tick's time and CPU. The paths that drop a request have nowhere to count it, because the run that marked it may be over and its dump about to be read.

**Change**
Carry the tick's timestamp and CPU in `StackSampleRequest`, and record those. For the count, a decision for the user: count a request dropped under a run that still matches (a killed thread) as skipped on the recording CPU, and state in the dump format that requests outstanding at stop are lost; or reserve the count at the tick and take it back on success.

**Reference**
Linux perf takes a sample's time and CPU in the PMU interrupt (`perf_prepare_sample()`), and copies the user stack there with `copy_from_user_nmi()`, which does not fault, so nothing about the sample is deferred.

**Acceptance**
A host test that a request's timestamp and CPU reach the record unchanged; the sampler stress test's checks still pass on both images.

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
