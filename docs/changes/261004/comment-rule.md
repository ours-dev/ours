# comment-rule changes

## Title: kernel/mm's comments describe the memory manager in its own terms, without naming other systems (2026-10-04)

**Files**
- `kernel/mm/BUILD.gn` — the two comments on the user_copy dependency and include path say what read_user()/write_user() need, not which foreign header they copy
- `kernel/mm/cpu_local.cpp`, `kernel/mm/include/ours/mm/cpu_local.hpp` — the per-CPU handle scheme, the %gs base, the laundering asm, the walk over every CPU with storage, ScopedInstall, PerCpu, Pinned and Current lose their "this is Linux's ..." and "Zircon's ... does the same" sentences; where one carried a reason (a per-CPU sum must visit offline CPUs, PerCpu never offers a dereference) the reason stays in this tree's words
- `kernel/mm/early_mem.cpp` — buddy release, bootmem retirement and init-area reclaim say what they do and why; the order of taking a frame off the reserved books now names unwire_frame() instead of a foreign function
- `kernel/mm/include/ours/mm/gaf.hpp`, `object_cache.hpp`, `pm_frame.hpp`, `memmap.hpp`, `memmap_init.hpp`, `page_queues.hpp`, `pm_node.hpp`, `pm_zone.hpp`, `pmm.hpp`, `priv/vflat_memmap.hpp`, `priv/zone_iterator.hpp` — allocation flags, slab options, frame roles and flags, memmap, page queues, NUMA nodes and zones: every "Linux's X" / "Zircon's Y" equivalence is gone, and the flags that were removed earlier are listed by what they meant rather than by the foreign names they copied
- `kernel/mm/include/ours/mm/vm_area.hpp`, `vm_area_or_mapping.hpp`, `vm_aspace.hpp`, `vm_cow_pages.hpp`, `vm_fault.hpp`, `vm_object.hpp`, `vm_object_paged.hpp`, `vm_object_physical.hpp`, `vm_page_map.hpp` — the VM object, area, mapping and address-space headers drop their "Zircon: Foo::Bar()" trailers; where the trailer was the only statement of a design choice (one mutex per tree rather than one per node, the window living on the object, chunked page-map keys as a missing amortisation) the choice is now stated directly
- `kernel/mm/init_vmm.cpp`, `memmap_init.cpp`, `object_cache.cpp`, `page_queues.cpp`, `pm_node.cpp`, `pm_node_init.cpp`, `pm_zone.cpp`, `pmm.cpp`, `sparse_memmap.cpp`, `vm_area.cpp`, `vm_area_or_mapping.cpp`, `vm_aspace.cpp`, `vm_cow_pages.cpp`, `vm_mapping.cpp`, `vm_object_paged.cpp` — the same sweep over the definitions: lock orders, error orders, the commit loop, the copy walk and the fault path give their reasons in this tree's terms

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of kernel/mm was sent to Linux's `mm/` or Zircon's `kernel/vm/` to learn why a lock is taken with interrupts off, why a flag exists or why a loop is shaped the way it is, and several comments gave the other system's behaviour as the only reason for this one's.

**Cause**
The comment rule adopted on 2026-10-04 (CLAUDE.md: code comments say what a thing is and why, in this codebase's own terms) moves every comparison with another system into the change record's Reference field and the design documents. kernel/mm was written by reading Zircon's VM and Linux's page allocator side by side, and its comments recorded that reading as they went: 183 comment lines in 39 files named one of the two. That habit hid information in three ways, and each needed a different rewrite rather than a deletion.

First, a comparison was sometimes the only place a fact about this code was written. `unwire_frame()`'s header said reclaiming physboot's identity-map tables "would need this to accept Role::Mmu, the case Linux's free_pagetable() tells apart with PageReserved()"; with the name gone, the sentence still has to say what the second half of that work is here (the page-table free path telling a boot table apart by its Reserved flag), or the repair item it points to loses half its plan. The same held for the Reserved flag (a bit, not a role, so that a boot page table can be Mmu and Reserved at once), for clear_reserved() (it comes before the free because PmNode::free_frames() asserts the frame is not reserved), and for who answers BadState when a clone meets a pinned page (the calling object, since pinning is the object's and not the pages').

Second, "as Zircon does" was often the stated reason for a rule that has a reason of its own. The zone and node-cache locks are taken with interrupts off because a frame or object freed from an interrupt must not meet the lock held underneath it, not because Linux's zone->lock is; a zero-length unmap is refused before the lock because the answer depends on nothing the lock guards, not because VmAddressRegion::Unmap() refuses it there. A reader judging whether the rule still holds after a change needs the real reason, and the comparison let it go unwritten.

Third, some comparisons described a design alternative that was not taken, and that alternative is real information about this design's limits. VmCowPages::HierarchyState's comment recounted a Zircon commit that went from one mutex per tree to one per node; what it means for this code is that one lock per node with a lock order is the known way out if the single mutex contends, so that is what the comment now says. The page map's note that Zircon keys its tree by 16-slot chunks becomes the statement that chunked keys are the amortisation missing here, and the commit loop's "half-copy of Zircon's CommitRangeInternal" becomes the one reason such a loop exists (waiting on a page source with the lock dropped, then advancing), which is what must come back with the first pager-backed VMO.

Sentences that were nothing but a comparison ("Zircon counterpart: VmAspace::Destroy()", "Linux prints the same fact from mem_init_print_info()") were dropped; the paragraph around each already said what the thing does.

**Change**
Comment text only. No code, identifier, string or build rule changed: a token comparison of every touched file at HEAD and after, with comments stripped, is identical. Typical rewrites:

```cpp
// Before (gaf.hpp)
/// compacts and tries again rather than failing. Linux's
/// __GFP_DIRECT_RECLAIM. Zircon's nearest is PMM_ALLOC_FLAG_CAN_WAIT,
/// which lets the PMM answer SHOULD_WAIT and leaves the waiting to the
/// caller. Reclaim and compaction are stubs today, ...
// After
/// compacts and tries again rather than failing. The allocator does the
/// waiting, not its caller. Reclaim and compaction are stubs today, ...

// Before (vm_cow_pages.hpp, HierarchyState)
/// Zircon counterpart: VmHierarchyState, which every VmCowPages held as
/// `hierarchy_state_ptr_` until commit 2dad682fe14 replaced it with one
/// lock per node and a lock order (...). One mutex per tree is
/// decision 3 of the vmo-snapshot-child design: the simplest thing that is
/// correct. Every process that clones one program contends on it, ...
// After
/// One mutex per tree is decision 3 of the vmo-snapshot-child design: the
/// simplest thing that is correct, where the alternative is one lock per
/// node and a lock order. Every process that clones one program contends on
/// the one mutex, ...

// Before (pm_zone.cpp)
/// The zone lock is taken with interrupts off, as Linux takes zone->lock
/// (spin_lock_irqsave in rmqueue_bulk() and free_one_page()).
// After
/// The zone lock is taken with interrupts off, on the allocation and the
/// free path alike.
```

**Reference**
The comparisons these comments carried, kept here so they are not lost now that the code no longer names them. Linux (`mm/`, `arch/x86/`): `__GFP_DIRECT_RECLAIM`, `__GFP_THISNODE`, `GFP_KERNEL`/`GFP_USER`, `ZONELIST_FALLBACK`/`ZONELIST_NOFALLBACK`, `build_zonelists()`, `find_next_best_node()`, `num_online_nodes()`, `numa_init()`, `init_cpu_to_node()`, `dummy_numa_init()`, `zone_sizes_init()`, `free_area_init()`, `build_all_zonelists()`, `__zone_watermark_ok()`, `rmqueue_bulk()`, `free_one_page()`, `__free_pages_memory()`, `__free_memory_core()`, `mem_init_print_info()`, `memblock_free_all()`, `memblock_discard()`, `free_initmem()`/`kernel_init()`, `free_reserved_area()`/`free_reserved_page()`, `ClearPageReserved()`, `adjust_managed_page_count()`, `alloc_low_pages()`, `free_pagetable()`, `PG_reserved` and the other page flags, `page_type`, `pfn_valid()`, `sparse_init()`, `SPARSEMEM_VMEMMAP`, `memmap_alloc()`/`__init_single_page()`, `inc_max_seq()`/`inc_min_seq()` (MGLRU), SLUB's `kmem_cache_node::list_lock`, `SLAB_CACHE_DMA`, `SLAB_NO_MERGE`, `SLAB_RECLAIM_ACCOUNT`, `slab_nodes`, the freelist offset and `cmpxchg_double` on (freelist, counters), `current_gfp_context()`, `task_lock()`, `__per_cpu_offset[]`, `__addr_to_pcpu_ptr()`, `pcpu_chunk_addr_search()`, `RELOC_HIDE()`, `for_each_possible_cpu()`, `get_cpu_ptr()`, `local_lock_irqsave()`, `this_cpu_read_stable()`, `cpu_kernelmode_gs_base()`, `SAVE_AND_SET_GSBASE` in `entry_64.S`, `mmap()`'s `MAP_FIXED`, `mseal()`. Zircon (`zircon/kernel/vm/`, `object/`, `arch/x86/`): `VmCowPages::{CreateCloneLocked, CloneCowPageLocked, FindPageContentLocked, AddNewPageLocked, ReleaseOwnedPagesLocked, MergeContentWithChildLocked, FindParentAndRangeForCloneLocked, ClampedLimit}`, `LookupCursor::{RequirePage, RequireReadPage, RequireOwnedPage, RequireResult}`, `VmHierarchyState` (and commit 2dad682fe14), `VmObjectPaged::{CreateFromWiredPages, CreateChildSlice, CreateClone, ReadWriteInternal, ReadUser, WriteUser, CommitRangeInternal, cow_pages_locked}`, `VmObject::{MAX_SIZE, ReadUser, AddMappingLocked, RangeChangeUpdateLocked}`, `VmPageList` (16-slot `VmPageListNode`) and `VmPageOrMarker`, `VmAddressRegion::{CreateSubVmarInternal, CreateSubVmarInner, Unmap, UnmapInternalLocked, DestroyLocked, FindMappingLocked, is_in_range}`, `VmAddressRegionOpChildren`, `VMAR_FLAG_SPECIFIC`, `VMAR_FLAG_CAN_MAP_SPECIFIC`, `ZX_VM_MAP_RANGE`, `VmMapping::{Activate, PageFault, PageFaultLockedObject, DestroyLockedObject}`, `VmAspace::{Init, Destroy, PageFault, PageFaultInternal}` with its `CriticalMutex`, `vdso_code_mapping_`, `VMM_PF_FLAG_INSTRUCTION`, `vm_page_state::{ALLOC, FREE, MMU, WIRED}`, `PmmNode::InitReservedRange()`, `pmm_unwire_page()`, `PMM_ALLOC_FLAG_CAN_WAIT`, `X86PageTableImpl::AllocatePageTable()`, the page queues' `kDefaultMinMruRotateTime`/`kDefaultMaxMruRotateTime`, MRU and LRU threads and `ProcessLruQueue`, `percpu::WithCurrentPreemptDisable()`, `arch_get_current_thread()`, the NMI path in `exceptions.S`.

**Verification**
A token comparison of all 39 files with comments stripped, HEAD against the working tree: identical. `scripts/format_cpp.py --check` clean on every changed C++ file; `scripts/format_gn.py --check` reports `kernel/mm/BUILD.gn` "unsafe", exactly as it does for the file at HEAD (a pre-existing property of the file, not of this change). clang-tidy on the 17 touched sources against `out/bringup.x86`: no diagnostic on a changed line (the 72 in kernel/mm all sit on untouched code). boot_check: build ok, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 0 failed). Host tests: `frame-buddy-smoke` OK, `protection-ranges-smoke` all passed. Kernel test on the dbg image: `vmosnapshot.test` ok, 205 checks. The sweep grep, `grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over kernel/mm, finds nothing.

**Deliberately not done**
No identifier was renamed, though several still carry the other system's vocabulary (`PmFolio`, `frame_to_folio`, `VmCowPages`): identifiers are outside rule 2. Comments that compare with nothing were left alone even where their prose could be tighter. kernel/mm's documents (none in this directory) and the other directories' comments belong to other groups' sweeps.

**How it was found**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over the C++, GN and Python files of kernel/mm (183 hits in 39 files), each hit read in context with the code it describes; every rewrite that states a fact the comparison used to carry was checked against the code (unwire_frame() clearing Reserved and both reserved counts, PmNode::free_frames()'s assertion, the IrqSave guards on the zone and node-cache locks, find_page()'s loop, the destructor's drain order). A second search for foreign artefacts without the system's name (`memblock`, `pg_data_t`, `SLUB`, `ZX_`, `VMAR`, `vm_page_t`, `.cc` paths) found none left.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is "a frame freed from an interrupt must not meet the lock held underneath it" a better reason for the zone lock's IrqSave than "as Linux takes zone->lock"?
2. When a comparison described a design the code did not take (one lock per node instead of one per tree), what must the comment still say once the other system's name is gone, and why?
3. Why does the Reserved page flag need to be a bit rather than a role, and which kind of frame shows it?
