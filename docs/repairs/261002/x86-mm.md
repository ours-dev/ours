# x86-mm outstanding repairs

## 1. A change to the kernel address space is not shot down on other CPUs

**Severity**
Medium: wrong on a path the kernel runs today, but only when another CPU has used the range just before it is unmapped, which nothing is known to do.

**Symptom**
Latent. After `vfree()` or any other unmap in the kernel address space, a CPU other than the one that unmapped can keep a translation of the freed range and read or write the frame's next owner through it.

**Cause**
`ArchVmAspace::invalidate()` asks every CPU in `m_active_cpus` for a user address space, and for a kernel one only invalidates this CPU's TLB, as the code always did. A kernel address space's entries are Global and aliased into every user address space (`PageTable::alias_to()`), so a CPU that switches root keeps them: the active mask says nothing about who holds them, and every online CPU would have to be asked. And some kernel unmaps run with interrupts off, where `IpiService::sync_exec()` cannot be used (it spins on answers that come from interrupt handlers, and asserts interrupts are on).

**Change**
Ask every online CPU for a kernel address space. Where the caller has interrupts off, defer: queue the range and flush it before the frames are freed, which is what Zircon's `VmAspace` does by freeing pages only after the `ConsistencyManager` has finished, and what Linux does with `flush_tlb_kernel_range()` before `vfree()` returns the pages (`vm_unmap_aliases()`, lazy purging). First list which kernel unmaps run with interrupts off.

**Reference**
Zircon `X86PageTableBase::ConsistencyManager::Finish()`, which shoots down every CPU for a global entry; Linux `flush_tlb_kernel_range()`.

**Acceptance**
A kernel test that maps a kernel page, touches it from a second CPU, unmaps it from the first and checks that the second CPU faults on it (a capturing access, as `vmosnapshot.test`'s shootdown check does for a user one).

## 2. Every shootdown is one round of IPIs that invalidates page by page

**Severity**
Low: a performance cost, nothing wrong.

**Symptom**
Latent cost, not measured. An unmap or protect of n pages sends one round of IPIs and runs n `invlpg` on every CPU in the mask, however large n is; a clone that takes Write off several mappings sends one round per mapping.

**Cause**
`ArchVmAspace::invalidate()` is called once per `unmap()`/`protect()` and loops `invlpg` over the range. Linux switches to a full flush past a ceiling (`tlb_single_page_flush_ceiling`, 33 pages), and both Linux and Zircon batch the ranges of one operation into one shootdown.

**Change**
Measure first, with the user (CLAUDE.md, performance work): time a clone of an object mapped in N address spaces on 4 CPUs, and an unmap of a large range, with the IPI count and the time each takes. Candidates: a full flush past a ceiling; batching the ranges of one operation (Zircon's `ConsistencyManager`, Linux's `mmu_gather`); asking only CPUs that may hold the pages (the mask already narrows to the address space).

**Reference**
Linux `flush_tlb_mm_range()` and `tlb_single_page_flush_ceiling`; Zircon `X86PageTableBase::ConsistencyManager`.

**Acceptance**
The measurement above, before and after, agreed with the user.
