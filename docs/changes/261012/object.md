# object changes

## object_get_info answers ProcessMaps: a process's address space, region by region and mapping by mapping (2026-10-12)

**Files**
- `system/public/ours/object_info.hpp` — `InfoTopic::ProcessMaps = 13`, `InfoMapsType`, `InfoMapsMapping` and `InfoMaps` (96 bytes, pinned by static_asserts); the topic comment says which topics are arrays.
- `system/public/ours/syscalls.gni` — `object_get_info`'s documentation: ProcessMaps, and a short buffer for an array topic.
- `kernel/lib/syscall/src/object.cpp` — `multi_record_result()`, and the ProcessMaps case (Inspect on a process).
- `kernel/object/process_dispatcher.cpp`, `.hpp` — `ProcessDispatcher::get_maps()`, two walks around one allocation; the record-building visitor.
- `kernel/object/vmo_dispatcher.cpp` — a VmoDispatcher gives its object its koid.
- `kernel/mm/vm_aspace.cpp`, `vm_aspace.hpp` — `VmAspace::base()`, `size()`, `for_each_node()`.
- `kernel/mm/vm_area.cpp`, `vm_area.hpp` — `VmArea::NodeEnumerator`: areas and mappings, depth first, with depth.
- `kernel/mm/include/ours/mm/vm_mapping.hpp` — `vmo()`, `vmo_offset()`, `for_each_protection_range_locked()`.
- `kernel/mm/include/ours/mm/vm_object.hpp`, `vm_object_paged.hpp`, `vm_object_paged.cpp`, `vm_cow_pages.hpp`, `vm_page_map.hpp` — `VmObject::get_name()`, `user_id()`, `set_user_id()`, `committed_pages()`; the paged object counts its own pages in a range.
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp`, `types.hpp` — `name()`; a forward declaration.
- `system/app/shell/shell_tests.cpp` — `check_process_maps()`.

**Symptom**
None, a new interface. No call could list another process's mappings, so a profiler had no way to find another process's code (`docs/repairs/261007/sampler.md` item 4, option A, `.agents/tasks/process-introspection/DESIGN.md`).

**Cause**
The step every later one needs: from a process handle to "which VMO is mapped where, with which protection". The pieces existed inside `kernel/mm` (the VMA tree, mappings, protection ranges), with no way out through a dispatcher, and two facts were missing altogether: an enumeration that yields regions as well as mappings (`MappingEnumerator`'s comment said it would get one when a caller came), and a koid on a VmObject, without which two mappings of one VMO cannot be told to belong together.

**Change**
The record is Zircon's v1 layout (`zx_info_maps_v1_t`, topic 13): name, base, size, depth, type, and for a mapping its protection, VMO koid, VMO offset and committed pages. Later Zircon layouts add memory-accounting fields this kernel cannot fill honestly, so their topic values are not claimed.

The walk is depth first in address order: the address space at depth 0, the root region at 1, each child one deeper; a mapping whose protection differs across its range is one record per range, as Zircon's `AspaceEnumerator` writes them. `VmArea::NodeEnumerator` walks the tree the way `MappingEnumerator` does, climbing by re-finding the finished area by its base rather than keeping a stack, so nesting costs nothing.

The lock is the design point. An address space has one lock over its whole tree, a non-recursive mutex. `get_maps()` holds it only to walk:

```cpp
auto counting = MapsPass{nullptr, 1};                  // walk 1: count, under the lock
m_aspace->for_each_node(&counting, &visit_maps_node);
records->reserve(std::min(max, counting.count));       // allocate, without it
auto filling = MapsPass{records, 0};                   // walk 2: fill, under the lock
m_aspace->for_each_node(&filling, &visit_maps_node);
*avail = filling.count;
// the caller copies the records out, without it
```

A user copy under that lock could fault, and resolving the fault takes the faulting process's own aspace lock: a process asking about itself would deadlock on its own mutex, and two processes asking about each other would take the two locks in opposite orders. The second walk counts again, because the tree can change between the walks; it never outgrows the room, since it keeps a record only while there is capacity. Counting a mapping's committed pages takes its VMO's lock, which nests inside the aspace lock in the documented order, and is done only for a record that will be kept.

An array topic answers Ok for a short buffer, with `actual < avail`, as Zircon's do; `single_record_result()` keeps BufferTooSmall for the one-record topics. A VmoDispatcher gives its object its koid at construction, before its first handle exists; the first wrapper keeps it.

Rejected: Zircon's per-record lock drop, which needs an enumerator that survives dropping the lock (ours's does not); copying out under the lock with fault capture and a pause and resume, the most code for the same safety.

**Reference**
Zircon `ZX_INFO_PROCESS_MAPS_V1` (`system/public/zircon/syscalls/object.h`), `ProcessDispatcher::GetAspaceMaps()` (`kernel/object/process_dispatcher.cc`), `GetVmAspaceMaps()` and `AspaceEnumerator` (`kernel/object/diagnostics.cc`), `VmObject::set_user_id()`.

**Verification**
- clang-format clean on the changed lines; clang-tidy (`out/bringup.x86`) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `53d25d37` plus only this change: build ok, `dbg PASS 36 passed, 3 skipped, 0 failed`, `rel PASS 34 passed, 8 skipped, 0 failed, 6/6 required` (one check more on each), host tests 144 passed. Both log `shell: process maps checks passed`: the address space first at depth 0; `check_process_maps` lies in a read-and-execute mapping of a VMO with a koid, below the root region; the same VMO is mapped at offset 0; one record's room gives `actual` 1 and the full `avail`; no Inspect is AccessDenied; a channel is BadType.
- With the koid line of `VmoDispatcher`'s constructor removed: `rel FAIL`, `shell: ProcessMaps: the code's VMO has no koid`.

**Deliberately not done**
- VMO names: no VMO is named today, so a mapping's record has an empty name, as Zircon's would for an unnamed VMO.
- The memory-accounting layouts (Zircon v2, v3).

**How it was found**
None, a new interface.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why may the maps walk not copy a record to user memory while it holds the address space's lock, even with fault capture switched off?
2. Why does the second walk count again instead of trusting the first count, and what keeps it from writing past the room?
3. Why does a profiler need the VMO's koid in each record to find a program's ELF header?

## process_read_memory: a debugger's read of another process's memory (2026-10-12)

**Files**
- `system/public/ours/syscalls.gni` — the `process_read_memory` entry.
- `system/public/ours/syscall.hpp` — `MAX_PROCESS_READ_MEMORY`, 64 MiB.
- `kernel/lib/syscall/src/process.cpp` — `ours::syscall::process_read_memory()`: arguments, then Read and Write on the process.
- `kernel/object/process_dispatcher.cpp`, `.hpp` — `ProcessDispatcher::read_memory()`.
- `kernel/mm/vm_aspace.cpp`, `vm_aspace.hpp` — `VmAspace::find_mapping()`, a referenced mapping out of the locked tree.
- `kernel/mm/vm_mapping.cpp`, `vm_mapping.hpp` — `vmo()` returns a reference of the caller's own, defined out of line.
- `system/app/shell/shell_tests.cpp` — `find_program_mappings()`, shared with the maps check, and `check_process_read_memory()`.
- `docs/repairs/261012/mm.md` — item 1, the zero page a read commits.

**Symptom**
None, a new interface. With ProcessMaps a profiler knows where another process's ELF header is mapped but cannot read it.

**Cause**
The second piece of outside-in symbolization (`.agents/tasks/process-introspection/DESIGN.md`): from a process handle and an address to the bytes there.

**Change**
The read goes through the mapping's object, not the target's page tables, as Zircon's does: the mapping that holds the address is found under the target's aspace lock (`find_mapping()`, NotFound otherwise, BadState once the space is destroyed) and referenced, the lock is dropped, the length is cut at the mapping's end, and the object is read with `read_user()`:

```cpp
ustl::Arc<mm::VmMapping> mapping;
m_aspace->find_mapping(va, &mapping);                  // under the target's lock
auto const into = va - mapping->base();
auto const length = std::min<usize>(size, mapping->size() - into);
auto const offset = mapping->vmo_offset() + into;
// user range checked, then, with no lock held:
mapping->vmo()->read_user(out, VirtAddr(offset), length);
```

No lock is held for the read, because it copies into the caller's buffer, which may fault, and resolving that takes the caller's own aspace lock: under the target's lock, a process reading itself would deadlock. The references keep the mapping and its object alive meanwhile; a mapping unmapped after the lookup is still read from the object it showed, the race Zircon's read has too. The user range is checked before the first page is read, as `vmo_read` checks it, since reading an untouched page commits a zeroed one in the target (`docs/repairs/261012/mm.md` item 1). Read and Write on the process are Zircon's debugger rights.

`VmMapping::vmo()` now hands out a reference of the caller's own (`ustl::Arc` by value), as Zircon's does, because `ustl::Arc`'s const is deep: through a const reference to the member no non-const call can be made on the object. It is defined in `vm_mapping.cpp`, since copying the reference needs `VmObject`'s whole type and `vm_object.hpp` includes `vm_mapping.hpp`.

**Reference**
Zircon `sys_process_read_memory()` (`kernel/lib/syscalls/process.rs`): READ|WRITE, at most 64 MiB, the read cut at the mapping's end, read through the VMO.

**Verification**
- clang-format clean on the changed lines; clang-tidy reports nothing on them (the shell against the worktree's own build, whose generated vDSO header has the new call).
- In `.worktrees/wip-proc-self` at baseline `2fcdf325` plus only this change: build ok, `dbg PASS 37 passed, 3 skipped, 0 failed`, `rel PASS 35 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed. Both log `shell: process read memory checks passed`: the ELF magic at the start of the offset-0 mapping; this function's 16 bytes as they are in memory; a 64-byte read starting 16 bytes before the code mapping's end returns 16; address 0 NotFound; 0 bytes and past the limit InvArgs; no Write AccessDenied; a channel BadType.
- With the cut at the mapping's end removed: `rel FAIL`, `shell: process_read_memory: a read is not cut at its mapping's end`.

**Deliberately not done**
- `process_write_memory`: nothing needs it yet.
- Reading across mappings: Zircon cuts at the mapping's end too (fxbug 42106495).

**How it was found**
None, a new interface.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the read go through the mapping's object rather than the target's page tables, and what does that make of the mapping's protection?
2. Why must the target's aspace lock be dropped before the copy, and what keeps the mapping and its object alive after it is?
3. What does a read of an untouched page cost the target here, and how does Zircon avoid that cost?

## A job lists its processes: object_get_info JobProcesses, and object_get_child (2026-10-12)

**Files**
- `kernel/object/include/ours/object/job_dispatcher.hpp`, `kernel/object/job_dispatcher.cpp` — the job's process list (`ProcessDispatcher::JobList`, under the job's lock) and `add_process()`, `remove_process()`, `get_process_koids()`, `lookup_process()`; the header now includes the process's, not the other way round.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — `m_job_hook`; listed at the end of `create()`, off the list in `finish_dead_transition()` and in the destructor; `is_unpublished()`, `lookup_thread()`.
- `kernel/object/include/ours/object/dispatcher.hpp` — `handle_count()`.
- `system/public/ours/object_info.hpp` — `InfoTopic::JobProcesses = 9`.
- `system/public/ours/syscalls.gni` — the `object_get_child` entry; `object_get_info`'s documentation.
- `kernel/lib/syscall/src/object.cpp` — the JobProcesses case, and `ours::syscall::object_get_child()`.
- `system/app/shell/shell_tests.cpp` — `check_job_processes()`, and the ended control process's checks in `check_object_get_info()`.

**Symptom**
None, a new interface. A process held a reference to its job, but the job knew nothing of its processes (the comment on `m_job` said the list would come with the first reader), so nothing could reach the processes running under a job.

**Cause**
The third piece of outside-in symbolization (`.agents/tasks/process-introspection/DESIGN.md`): a profiler holding a job must find the processes already running under it, by koid, and get a handle to each.

**Change**
The list is intrusive and raw, through a hook on the process, guarded by the job's lock. Its invariant: every listed process is safe to read under that lock. It holds because a process leaves the list before its memory goes, and leaving takes the job lock that every reader holds:

```cpp
ProcessDispatcher::~ProcessDispatcher() {
    m_job->remove_process(*this);   // first, before anything of it goes
    ...
}
```

A process joins at the end of `create()`, when nothing can fail any more, and leaves when it dies (`finish_dead_transition()`, outside the process lock, as the Job -> Process order requires) or, if it never died, when it is destroyed; removal is idempotent through `is_linked()`. Listing hides a process its creator has not handed out yet, still Initial with no handle (Zircon's filter); the check takes the process lock under the job lock, the settled Job -> Process order. `get_process_koids()` counts under the lock, allocates without it, and fills under it again, as the maps walk does.

`object_get_child` follows Zircon's order: the parent with Enumerate, the rights asked a subset of the parent's (or SameRights), then a job's process or a process's thread by koid. `lookup_process()` takes the reference with `try_adopt()` under the job lock, since a listed process may have lost its last reference already, and never drops one there: dropping the last would run the destructor, which takes the same lock. The new handle is made from a reference of its own (`Handle::make(Arc, rights)`), never a `KernelHandle`, whose destructor on a failed make would call `on_zero_handles()` and kill a process still Initial that other handles hold.

The include direction flips: `job_dispatcher.hpp` needs the process's list type, and `process_dispatcher.hpp` only ever held the job through an Arc, which a forward declaration serves; every file that uses the job already includes its header.

**Reference**
Zircon `ZX_INFO_JOB_PROCESSES` (`kernel/lib/syscalls/object_info.rs`), `JobDispatcher::EnumerateChildren()` and the hidden-process filter (`kernel/object/job_dispatcher_ffi.cc`), `sys_object_get_child()` (`kernel/lib/syscalls/object.cc`), `JobDispatcher::LookupProcessById()`, and the removal at Dead outside the process lock (`kernel/object/process_dispatcher.cc`).

**Verification**
- clang-format clean on the changed lines; clang-tidy (the worktree's build, whose generated headers have the new call) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `a983516f` plus only this change: build ok, `dbg PASS 38 passed, 3 skipped, 0 failed`, `rel PASS 36 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed. Both log `shell: job process checks passed`: the job lists this shell; one koid's room is Ok; a process made and held is listed and, once its last handle closes and it dies unstarted, is not; `object_get_child` by this shell's koid names it; rights past the job's AccessDenied; an unknown koid NotFound; no Enumerate AccessDenied for both calls; a channel AccessDenied; a process with no such thread NotFound. On dbg, the control probe, dead but still held, is neither listed nor found.
- With the removal at death taken out: `dbg FAIL`, `shell: the job still lists the control after it ended` and `object_get_child(job, the ended control) = Ok (expected NotFound)`. A process dropped by its last handle is destroyed at once, so only a dead process still held shows the removal at death; the first form of the test, which had none, passed the mutation.

**Deliberately not done**
- Child jobs and topic JobChildren: there is no `job_create`, so the root is the only job.
- A positive test of `object_get_child` for a thread: the shell holds no handle to its own thread (ort does not take ThreadSelf), and an unstarted thread is not yet on its process's list.

**How it was found**
None, a new interface.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What makes it safe to read a listed process under the job's lock without taking a reference to it?
2. Why may `lookup_process()` never drop a reference while it holds the job's lock?
3. Why is the handle `object_get_child` hands out made from an Arc rather than a KernelHandle?

## A job's debugger channels, told by ProcessStarting of every process that starts in the job (2026-10-12)

**Files**
- `system/public/ours/exception.hpp` — `ExceptionType::ProcessStarting` (0x8308), `ExceptionChannelOptions::Debugger` (1), `MAX_JOB_DEBUGGER_CHANNELS` (32)
- `kernel/include/ours/task/exception.hpp` — `to_string()` names ProcessStarting
- `kernel/lib/syscall/src/exception.cpp` — `task_create_exception_channel` binds a job's debugger channel; the job lookup replaces `names_a_job()`
- `kernel/object/include/ours/object/job_dispatcher.hpp`, `kernel/object/job_dispatcher.cpp` — `DebugExceptionate`, `bind_debugger()`, `for_each_debugger()`, `prune_debuggers_locked()`, the fixed array of up to 32
- `kernel/object/include/ours/object/exceptionate.hpp`, `kernel/object/exceptionate.cpp` — public `has_valid_channel()`
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp` — `offer_exception()` factored out of `deliver_exception()`, `report_process_start()`, `m_starts_process`, the raise in `trampoline()`
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — `job()`
- `system/app/shell/shell_tests.cpp` — `answer_process_starting()` and an optional `debuggers` argument to `run_probe()`, `check_job_debugger()`

**Symptom**
None, a new feature: step 5 of `.agents/tasks/process-introspection/PLAN.md`. A profiler that describes every process its records name has to learn of processes that start while it samples; until now nothing told anyone a process had started.

**Cause**
None, a new feature.

**Change**
The objects:
- A **debugger channel**: a channel whose kernel end sits in an `Exceptionate` the job keeps. `task_create_exception_channel(job, Debugger)` makes one; a job takes up to 32.
- **`DebugExceptionate`**: an `Exceptionate` that is also ref-counted, because a start sends on it without the job's lock while a bind may drop it from the job.
- **ProcessStarting**: a synthetic exception, `ExceptionInfo{pid, tid, 0x8308}` plus the exception's handle, sent to each debugger channel in turn.

The events, for one process start:
1. The starter calls `process_start`; `ThreadDispatcher::start()` records `m_starts_process = first_thread` and `attach_thread()` resumes the kernel thread.
2. The new thread enters `trampoline()`, and before interrupts go off for the last stretch to ring 3 it calls `report_process_start()`.
3. `JobDispatcher::for_each_debugger()` takes the job's lock, drops channels whose readers have gone, copies the rest (Arcs) into a stack array, and lets the lock go.
4. For each one in bind order, `offer_exception()` sends under the thread's lock (a kill cannot slip between "still Running" and "sent"), waits for the handle's close without it, and looks at the state again. The answer is ignored, except that a kill stops the walk.
5. The thread goes on to `process_pending_signals()` and `arch_enter_uspace()`; a process killed while it waited exits there.

The per-slot body of `deliver_exception()` became `offer_exception()`, so the fault path and the start path share one send-and-wait:

```cpp
// before: inline in deliver_exception()'s loop
Guard guard{get_lock()};
if (m_state != State::Running) { return Delivery::Killed; }
if (slot->send(exception) != Status::Ok) { continue; }
...
// after
switch (offer_exception(*slot, exception)) {
    case Offer::Unsent: continue;
    case Offer::PassedOn: told = true; continue;
    case Offer::Handled: return Delivery::Handled;
    case Offer::Killed: return Delivery::Killed;
}
```

What the syscall answers, after the rights check in every case: a job with `Debugger` binds; a job with `None` stays NotSupported (its own channel is not built); a process with `Debugger` is NotSupported (a process debugger channel is not built); a thread with `Debugger` is InvArgs (there is no such thing); any other option bit is InvArgs. The handles a debugger asks an exception for carry the job handle's rights, so a debugger gets no more than whoever bound it had.

Choices made:
- A fixed array of 32 Arcs plus a count, not an intrusive list: the most there can be is small and fixed, a visit copies them into an array of the same size anyway, and order is kept by compacting on prune.
- Dropped channels are moved into a local array and released after the job's lock, because the last reference closes the channel's kernel end, which runs `on_zero_handles()` and must not happen under a task's lock (the rule `Exceptionate::shutdown()` states).
- The exception is created lazily, at the first debugger: a process starting in a job nobody debugs pays one look at the list.
- The answer is ignored: the debugger is told, it does not decide. A debugger that wants the process stopped binds its process channel while the start is held, or kills it.

**Reference**
Zircon `JobDispatcher::CreateDebugExceptionate()` and `JobDispatcher::ForEachDebugExceptionate()` (prune, snapshot under the lock, visit without it), `ProcessDispatcher::OnProcessStartForJobDebugger()`, the `is_initial_thread_` check in `ThreadDispatcher::StartRoutine()`, `ThreadDispatcher::HandleSingleShotException()`; `ZX_EXCP_PROCESS_STARTING`, `ZX_EXCEPTION_CHANNEL_DEBUGGER`, `ZX_EXCEPTION_CHANNEL_JOB_DEBUGGER_MAX_COUNT` in `zircon/syscalls/exception.h`. Differences: Zircon walks the parent jobs as well (this kernel has only the root job), and keeps a `DoublyLinkedList<RefPtr<DebugExceptionate>>` and erases under the lock.

**Verification**
- Built in `.worktrees/wip-proc-self` at a319b06b plus only this change; clang-tidy clean on the changed lines.
- `boot_check.py`: dbg PASS, 39 passed (one more: "job debugger checks passed"), 4 skipped; rel PASS, 37 passed, 8 skipped; host tests 144 passed.
- The dbg checks: an unknown option bit InvArgs, a process NotSupported, the probe's first thread InvArgs; two debuggers both hear the probe's start, in bind order, the second silent while the first holds it; `exception_get_process` and `exception_get_thread` name the probe and its first thread; the probe then answers "unknown" with no exception on its process channel; with the first reader closed a second probe starts with only the second told; 32 bind, a 33rd is AlreadyBound, and after closing them one binds again.
- Mutations, dbg only: the call in `trampoline()` removed hangs the shell at the debugger read (FAIL, no end marker within 60 s); pruning disabled gives "fewer than the most a job takes bound" and "job debugger checks FAILED".

**Deliberately not done**
- A job's own exception channel and a process's debugger channel stay NotSupported: nothing needs them yet.
- ThreadStarting, ThreadExiting and a debugger hearing ordinary exceptions ahead of the thread's own channel: the profiler needs only process starts.
- Walking parent jobs: there is only the root job.
- The shell's wait for ProcessStarting is unbounded, as every exception wait there is: userspace waits take only 0 and infinite deadlines today, so a missing report shows as a hang, which `boot_check.py` reports.

**How it was found**
None.

**The user's prediction**
None (delegated: "你看着来，尽可能完善的做").

**Three questions you should be able to answer**
1. Why is the debugger list copied out under the job's lock and visited without it, and what would go wrong if `offer_exception()` ran with the job's lock held?
2. Why may a pruned `DebugExceptionate` not be released while the job's lock is held?
3. What stops a process that is killed while its first thread waits in ProcessStarting from running any user code?
