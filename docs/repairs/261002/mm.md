# mm outstanding repairs

## 1. A dead thread's kernel stack is never given back

**Severity**
Medium: a leak on a path the kernel runs today, at every thread exit.

**Symptom**
Every thread that is destroyed leaves its 16 KiB kernel stack behind: the pinned frames, the `VmObjectPaged` and `VmMapping` behind them, and the range of kernel virtual addresses. Not measured; it grows with the number of threads ever destroyed.

**Cause**
`Stack::init()` (`kernel/mm/stack.cpp:22-50`) makes the mapping with `create_mapping()` in the kernel's root VMA, which links it into the root's `VmaSet`; `VmaSet::insert()` takes a reference of its own (`vm_area_or_mapping.cpp:93`), released only by `VmaSet::erase()`, that is by destroying the mapping. `~Stack()` is defaulted (`kernel/mm/include/ours/mm/stack.hpp:20`): it drops the `Stack`'s own `Arc` and nothing else. `Thread::destroy()` (`kernel/kernel/task/thread.cpp:432-456`, run from a DPC) runs `~Thread()`, which runs `~Stack()`, and nothing ever destroys the mapping. The comment above `~VmCowPages()` (`kernel/mm/vm_cow_pages.cpp:38-46`) already notes that kernel stacks are never destroyed.

Fixing it gives the stack's range back to a lowest-address-first allocator while other CPUs routinely hold translations of a thread's stack (it migrates; `sync_exec()` requests live on it), and the next thread gets the same range. That is safe only because a kernel unmap is shot down on every online CPU, since 2026-10-02 (`ArchVmAspace::invalidate()`).

**Change**
Destroy the mapping when the stack goes (`~Stack()` or `Thread::destroy()` calls `destroy()` on it); the unmap already reaches every CPU. Zircon also puts guard pages around each kernel stack; this one has none, and that is a separate item if wanted.

**Reference**
Zircon `KernelStack::Teardown()` (`zircon/kernel/vm/kstack.cc:248`), which destroys the stack's mappings and VMARs when the thread is freed.

**Acceptance**
A command that spawns and joins many threads shows the free-frame count back where it started; `vmosnapshot.test` and the shell's checks still pass on both images.

## 2. VmArea::create_common() drops the status of activate(), so a sub-region can be Ok and not linked

**Severity**
Medium: reachable only once allocation can fail (`docs/repairs/260908/heap.md` item 3).

**Symptom**
Latent, cannot be triggered today. `create_subvma()` can answer Ok for a sub-region that is marked Active but is not in its parent's set. Since `vma_allocate` gives sub-regions CanMap bits, such an orphan can hold mappings. The parent's `find_spot()`, the fault lookup and `VmAspace::destroy()` never see them, so a later placement can land over their page-table entries. When the orphan's last handle closes, `~VmaSet` runs with its mappings still on their VMOs' lists. That is a DEBUG_ASSERT in a debug build and the SafeLink trap in a release build.

**Cause**
`VmArea::create_common()` calls `self->activate()` and discards the status (`kernel/mm/vm_area.cpp:60`). `VmAreaOrMapping::activate()` marks the area active before `m_subvmas.insert()` (`kernel/mm/vm_area_or_mapping.cpp:50-53`). The insert can fail with OutOfMem, because the set is a node-allocating B-tree, and then it takes no reference. `heap.md` item 4 records the same defect for `VmMapping::activate()`. Before sub-regions could be mapped into, an orphan was an inert guard.

**Change**
Propagate `activate()`'s status out of `create_common()` and unwind the half-made area; extend the `heap.md` item 4 fix to `VmArea` in the same change.

**Reference**
Zircon `VmAddressRegion::Activate()` (`zircon/kernel/vm/vm_address_region.cc:651-675`) marks the region ALIVE only after `InsertRegion()` succeeds, and returns the insert's status, which `CreateSubVmarInner()` passes up (`:244`). `VmMapping::ActivateLocked()` (`zircon/kernel/vm/vm_mapping.cc:1422`) does the same for a mapping.

**Acceptance**
With allocation failure injectable, a failed insert makes `vma_allocate` answer OutOfMem and leaves nothing reserved; both images boot and the shell's checks pass.

## 3. A region's allowance is read without the address-space lock while teardown writes the same word

**Severity**
Medium: a data race under the C++ memory model, under a stated condition; harmless on x86 as compiled today.

**Symptom**
Latent, not observed. A read of a region's allowance can race with the death of the process that owns the address space.

**Cause**
`VmAreaOrMapping::permissions()` reads `m_vmaf & PermMask` from a plain member (`kernel/mm/include/ours/mm/vm_area_or_mapping.hpp:240-242`) with no lock. `VmAreaOrMapping::destroy()` read-modify-writes the same word, `m_vmaf &= ~VmaFlags::Active` (`kernel/mm/vm_area_or_mapping.cpp:56`), under the address-space lock from `VmAspace::destroy()`. A VMA handle outlives its process and can be transferred, so the reader can be a launcher calling `vma_allocate` on a child's root-region handle at the moment the child's last thread exits. The readers are `VmaDispatcher::allocate()` (`kernel/object/vma_dispatcher.cpp:269`), `VmaDispatcher::create()` (`:46`), `validate_vmaflags()` (`kernel/mm/vm_area.cpp:90`) and `create_mapping()` (`:141`). The invariant they rely on holds: a region's allowance never changes after it is made. The race is in sharing the word with state bits that do change.

**Change**
Keep the allowance apart from the state bits, as a const member set at construction, like Zircon's `flags_`. Then the readers need no lock. The other option is to read it only under the address-space lock, by moving the subset check into `create_subvma()`. That covers one reader and leaves the rest.

**Reference**
Zircon `VmAddressRegion::flags_` is const after construction, and `CreateSubVmarInner()` (`zircon/kernel/vm/vm_address_region.cc:122`) checks it under the region's lock.

**Acceptance**
No unlocked reader of a word that teardown writes: the allowance is a const member, or every read is under the lock. Both images boot and the shell's checks pass.
