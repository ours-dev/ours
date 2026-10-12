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
