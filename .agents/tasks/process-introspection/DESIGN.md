# process-introspection: let a profiler see every process's code from outside it

**Date**: 2026-10-12
**Status**: Accepted

## Problem
The decisions below were delegated by the user on 2026-10-12 ("你看着来，尽可能完善的做") and are open to overruling.

prof-fold names a user frame only when the sampled process printed its own module map (`PROFP` and symbolizer markup), and only `profile` does (`docs/repairs/261007/sampler.md` item 4). A system-wide profile shows every other process's frames as addresses. The profiler cannot learn another process's code from outside: ours has no call that lists a process's mappings, none that reads another process's memory, no way to reach the processes under a job, and no notice when a process starts. The user chose the outside-in approach (option A of 2026-10-11) and asked for the process-starting notice as well.

## Goals and non-goals
Goals:
- A holder of a process handle with Inspect lists its mappings (`object_get_info` topic ProcessMaps), and with Read and Write reads its memory (`process_read_memory`).
- A holder of a job handle with Enumerate lists the job's processes (topic JobProcesses) and gets a handle to one by koid (`object_get_child`).
- A program reaches the root job by path, as a protocol the root serves in `/svc` (`ours.kernel.RootJob`).
- A debugger bound to a job's debugger exception channel is told of every process that starts under it, before its first user instruction, and gets its handle from the exception (`ExceptionType::ProcessStarting`).
- `profile` uses all of it to print module markup for every process its samples name, so prof-fold names their user frames.

Non-goals, each addable later without changing these interfaces:
- `job_create`, child jobs and topic JobChildren: the root is the only job, so a walk up the job tree has one step.
- The memory-accounting fields of Zircon's current maps record (v3).
- Thread starting exceptions, a job's own (non-debugger) exception channel, `object_get_child` for a process's threads.
- `process_write_memory`.

## Design
Objects and interfaces, Zircon's names and values kept unless said:

- **ProcessMaps** (`InfoTopic::ProcessMaps = 13`, Zircon's `ZX_INFO_PROCESS_MAPS_V1`): an array of `InfoMaps` records (96 bytes, Zircon's v1 layout: name[32], base, size, depth, type, then for a mapping `mmu_flags`, `vmo_koid`, `vmo_offset`, `committed_pages`). Depth-first, pre-order: the address space (depth 0), then each Vma and mapping with its depth. Needs Inspect. Like every multi-record topic it answers Ok with `actual < avail` for a short buffer (Zircon `object_info_ffi.cc`); BufferTooSmall stays for single-record topics only.
  - Built by `ProcessDispatcher::get_maps()` over a new `mm::VmAspace` enumeration of areas and mappings.
  - Locking: the target's aspace lock (`VmAspace::m_lock`) is held only to walk, never across a user copy or an allocation. Two passes: count the nodes under the lock; allocate `min(count, max)` records without it; fill under the lock, counting `avail` again; copy out without it. A user copy under that lock would deadlock a process inspecting itself (the lock is not recursive) and two processes inspecting each other (opposite order).
  - `vmo_koid` needs a koid on `mm::VmObject`, set when a VmoDispatcher wraps it (Zircon's `set_user_id`).
- **process_read_memory(process, vaddr, buffer, size, actual)**: Read and Write on the process (Zircon `process.rs`). Finds the mapping that holds `vaddr` under the aspace lock (NotFound otherwise), takes its VMO and offset, cuts the size at the mapping's end, drops the lock, and reads the VMO through `read_user`. At most `MAX_PROCESS_READ_MEMORY` (64 MiB). Unlike Zircon, reading a never-written anonymous page commits a zero page in the target (ours's `read_user` path); recorded as a repair item.
- **JobProcesses** (`InfoTopic::JobProcesses = 9`, `KoId[n]`) and **object_get_child(handle, koid, rights, out)**: a job keeps a list of its processes, raw, guarded by the job's lock: a process joins at the end of `ProcessDispatcher::create()` and leaves when it dies (`finish_dead_transition()`, outside the process lock) or is destroyed. Enumeration hides a process still Initial with no handle (Zircon's filter). `object_get_child` needs Enumerate on the parent; SameRights means the parent's rights, a superset is AccessDenied. It takes the reference with `try_adopt` under the job lock and drops nothing under it: dropping the last reference there would run the destructor, which takes the same lock.
- **ours.kernel.RootJob**: one Get, answered with a SameRights duplicate of the root's root job, served by init like the `ours.resource` protocols, routed to the shell, whose namespace the profiler it starts inherits. Full rights, as Fuchsia's cpu_profiler uses `fuchsia.kernel.RootJob` rather than `RootJobForInspect`: reading memory needs Read and Write.
- **Job debugger channel and ProcessStarting**: `task_create_exception_channel(job, ExceptionChannelOptions::Debugger)` binds one of up to 32 debugger exceptionates on the job (`ZX_EXCEPTION_CHANNEL_DEBUGGER`, `JOB_DEBUGGER_MAX_COUNT`). When a process's first thread starts, it raises `ExceptionType::ProcessStarting` (0x8308) in its trampoline, before it enters user mode: for the process's job and each job above it (one today), it snapshots that job's debuggers under the job lock and sends to each in turn, waiting for each exception handle to close; the disposition is ignored (Zircon `HandleSingleShotException`). A debugger's `exception_get_process` gets the job handle's rights (Zircon's choice). Delivery stops once the thread is killed (Zircon goes on sending to the remaining debuggers, each of which then returns at once).
- **profile**: takes `ours.kernel.RootJob`, binds a debugger channel on it, describes every process the job lists and every one that starts while it samples (maps, the offset-0 mapping of each executable mapping's VMO for the ELF header, `process_read_memory` for the build ID note), and prints `PROFP` and markup for each process its records name, itself included through the same path.

Concurrency, the arguments the code relies on:
- Maps walk: the aspace lock is the one lock of a VMA tree; the walk reads nodes under it and nothing else touches user memory or allocates under it, so it nests only the VMO lock (for committed pages), the documented order.
- Job list: the invariant is "a listed process is alive enough to read its koid and state". It holds because a process leaves the list before its memory goes, and leaving takes the job lock that every reader holds. Enumeration takes Job then Process locks, the settled order (`docs/design/260923/userspace-interface.md`).
- Debugger snapshot: Arcs taken under the job lock keep each exceptionate alive across the sends, which run without the job lock (Job is above ThreadDispatcher and Exceptionate in the order, so sending under it would be legal, but would hold every process start in the tree behind one debugger).

## Alternatives considered
- B, kernel-recorded mapping events in the dump (Linux `PERF_RECORD_MMAP2`): puts a hook on the map path and changes the dump format; rejected on 2026-10-11.
- C, each program describes itself from ort: least work, but needs the console captured from boot and covers only ort programs; rejected by the user's choice of A.
- Zircon's per-record lock drop for the maps walk: needs an enumerator that survives dropping the lock, which ours's does not; the two-pass snapshot gets the same safety with one extra walk.
- `RootJobForInspect` only: cannot grant Read and Write to a child, so `process_read_memory` would be refused; a departure (`read_memory` needing only Read) was not taken.
- Using the JobDefault startup handle the shell already holds as the profiler's route to the root job: works today, but it is a stand-in that silently narrows once components get jobs of their own.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Approach | A outside-in; B mapping events; C self-description | C at first, then A | Not given | A, 2026-10-11, plus the process-starting notice |
| Maps record version | v1 (96 B, committed pages); v3 (accounting fields) | v1: v3's fields cannot be filled honestly | Not given | v1, 2026-10-12 (delegated) |
| Maps walk locking | Two-pass snapshot; per-record lock drop (Zircon); user copy under the lock with fault capture | Two-pass snapshot | Not given | Two-pass, 2026-10-12 (delegated) |
| process_read_memory rights | Read and Write (Zircon); Read only | Zircon's | Not given | Read and Write, 2026-10-12 (delegated) |
| Root job route | `ours.kernel.RootJob` (full); `RootJobForInspect`; JobDefault | RootJob | Not given | RootJob, 2026-10-12 (delegated) |
| Process rights a job debugger gets | The job handle's (Zircon); masked to the default process rights | Zircon's | Not given | Zircon's, 2026-10-12 (delegated) |
| ProcessStarting after a kill | Stop (here); keep sending (Zircon) | Stop: the remaining sends would return at once anyway | Not given | Stop, 2026-10-12 (delegated) |

## Testing and verification
- Shell checks, on both images where the handles exist: maps of the shell's own process include an executable mapping holding a known function and a mapping at VMO offset 0 of the same VMO; `process_read_memory` of that offset reads `\x7fELF`; rights and error cases (no Inspect, no Read/Write, an unmapped address, a size of 0, past 64 MiB); JobProcesses on the shell's job lists the shell's own koid and `object_get_child` by it gives a handle whose koid matches; a debugger bound on the job is told of a probe process's start with the right pid, gets its process, and the probe runs once the exception closes; two debuggers are told one after the other; a vanished debugger does not hold a start.
- `profile` on dev prints markup for several processes, and prof-fold names user frames of a process other than profile when it has samples.
- `ours-verify` before each commit; mutation checks where a test guards a line.

## Open questions
- Whether the hidden-process filter or registering on the first handle fits ours better, if Zircon's filter turns out awkward.
- The cost of the root job's lock on every process start, recorded rather than optimised.
