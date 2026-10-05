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
The comparisons these comments carried, kept here so they are not lost now that the code no longer names them. Linux (`mm/`, `arch/x86/`): `__GFP_DIRECT_RECLAIM`, `__GFP_THISNODE`, `GFP_KERNEL`/`GFP_USER`, `ZONELIST_FALLBACK`/`ZONELIST_NOFALLBACK`, `build_zonelists()`, `find_next_best_node()`, `num_online_nodes()`, `numa_init()`, `init_cpu_to_node()`, `dummy_numa_init()`, `zone_sizes_init()`, `free_area_init()`, `build_all_zonelists()`, `__zone_watermark_ok()`, `rmqueue_bulk()`, `free_one_page()`, `__free_pages_memory()`, `__free_memory_core()`, `mem_init_print_info()`, `memblock_free_all()`, `memblock_discard()`, `free_initmem()`/`kernel_init()`, `free_reserved_area()`/`free_reserved_page()`, `ClearPageReserved()`, `adjust_managed_page_count()`, `alloc_low_pages()`, `free_pagetable()`, `PG_reserved` and the other page flags, `page_type`, `pfn_valid()`, `sparse_init()`, `SPARSEMEM_VMEMMAP`, `memmap_alloc()`/`__init_single_page()`, `inc_max_seq()`/`inc_min_seq()` (MGLRU), SLUB's `kmem_cache_node::list_lock`, `SLAB_CACHE_DMA`, `SLAB_NO_MERGE`, `SLAB_RECLAIM_ACCOUNT`, `slab_nodes`, the freelist offset and `cmpxchg_double` on (freelist, counters), `current_gfp_context()`, `task_lock()`, `__per_cpu_offset[]`, `__addr_to_pcpu_ptr()`, `pcpu_chunk_addr_search()`, `RELOC_HIDE()`, `for_each_possible_cpu()`, `get_cpu_ptr()`, `local_lock_irqsave()`, `this_cpu_read_stable()`, `cpu_kernelmode_gs_base()`, `SAVE_AND_SET_GSBASE` in `entry_64.S`, `mmap()`'s `MAP_FIXED`, `mseal()`. Zircon (`zircon/kernel/vm/`, `object/`, `arch/x86/`): `VmCowPages::{CreateCloneLocked, CloneCowPageLocked, FindPageContentLocked, AddNewPageLocked, ReleaseOwnedPagesLocked, MergeContentWithChildLocked, FindParentAndRangeForCloneLocked, ClampedLimit}`, `LookupCursor::{RequirePage, RequireReadPage, RequireOwnedPage, RequireResult}`, `VmHierarchyState` (and commit 2dad682fe14), `VmObjectPaged::{CreateFromWiredPages, CreateChildSlice, CreateClone, ReadWriteInternal, ReadUser, WriteUser, CommitRangeInternal, cow_pages_locked}`, `VmObject::{MAX_SIZE, ReadUser, AddMappingLocked, RangeChangeUpdateLocked}`, `VmPageList` (16-slot `VmPageListNode`) and `VmPageOrMarker`, `VmAddressRegion::{CreateSubVmarInternal, CreateSubVmarInner, Unmap, UnmapInternalLocked, DestroyLocked, FindMappingLocked, is_in_range}`, `VmAddressRegionOpChildren`, `VMAR_FLAG_SPECIFIC`, `VMAR_FLAG_CAN_MAP_SPECIFIC`, `ZX_VM_MAP_RANGE`, `VmMapping::{Activate, PageFault, PageFaultLockedObject, DestroyLockedObject}`, `VmAspace::{Init, Destroy, PageFault, PageFaultInternal}` with its `CriticalMutex`, `vdso_code_mapping_`, `VMM_PF_FLAG_INSTRUCTION`, `vm_page_state::{ALLOC, FREE, MMU, WIRED}`, `PmmNode::InitReservedRange()`, `pmm_unwire_page()`, `PMM_ALLOC_FLAG_CAN_WAIT`, `X86PageTableImpl::AllocatePageTable()`, the page queues' `kDefaultMinMruRotateTime`/`kDefaultMaxMruRotateTime`, MRU and LRU threads and `ProcessLruQueue`, `percpu::WithCurrentPreemptDisable()`, `arch_get_current_thread()`, the NMI path in `exceptions.S`. Named in passing, and as worth comparing: Linux's `PageBuddy`, `MAX_PAGE_ORDER` and `__ffs()`, `PFN_UP()`/`PFN_DOWN()`, `memblock_alloc()`, `kernel_init_freeable()` and `__ref`, `num_node_state(N_ONLINE)`, `pg_data_t`'s `node_zones[]` and `node_zonelists[]`, `numa_distance[]`, `x86_cpu_to_node_map` behind `early_per_cpu()` and `cpu_to_node()`, `numa_set_node()` before `smp_init()`, `__alloc_pages()`/`alloc_pages_node()` against `alloc_pages()` and the mempolicy, `slab_node_online()`/`slab_node_offline()` under `slab_mutex`, `get_partial_node()`/`__slab_free()`, `pcpu_lock`, `memblock_alloc_try_nid_raw()` with `mm_zero_struct_page()`, `for_each_online_cpu()`, `preempt_disable()` then `this_cpu_ptr()` in `get_cpu_ptr()`/`put_cpu_ptr()`, `get_current()`, sparse's `__percpu`, `paranoid_entry`/`paranoid_exit` with `GET_PERCPU_BASE`, `mm/mm_init.c`; Zircon's `~VmAspace()` with `arch_aspace_.Destroy()`, `ArchVmAspace::HandoffPageTablesFromPhysboot()`, `ProcessDispatcher::vdso_code_address_` and `VDso::ValidSyscallPC()`, `VmAddressRegionDispatcher::op_children_from_rights()` with `ZX_RIGHT_OP_CHILDREN` and `sys_vmar_unmap()`, `VmAddressRegionEnumeratorType::{VmarsAndMappings, MappingsOnly}`, `VmAddressRegion::{CreateVmMapping, ReserveSpace}`, `ToVmarFlags()` (`vm.cc`), `region_list_get_alloc_spot_test`, `VmMapping`'s `CAN_MAP_*` flags, `VmMapping::{ActivateLocked, MapRange, AspaceRemoveWriteLockedObject, EnumerateProtectionRangesLockedObject}`, `ARCH_MMU_FLAG_PERM_EXECUTE`, `RangeChangeOp::RemoveWrite`, `VmObject::size_locked()`, `VmObjectPhysical`'s own lock, `VmCowPages::{is_hidden, MaybeDeadTransition}` and its `page_list_`, `paged_ref_`, `parent_offset_`, `parent_limit_`, `LookupCursor`'s `offset_`/`end_offset_` and `PageAsResultNoIncrement()`, `VmPageList::RemoveAllPages()`, `VmPageOrMarker::Zero`, `vm_get_zero_page()`, `vm_init_preheap()` with `pmm_alloc_page()`, `SnapshotType::OnWrite`, `CreatePhysVmo()` (`top/handoff.cc`), `SoftFault()`/`SoftFaultInRange()`, `lockdep::AssertNoLocksHeld()`, `TrimLength`, `ZX_ERR_{NOT_SUPPORTED, OUT_OF_RANGE, SHOULD_WAIT}`, `vm_page_t::object.{share_count, page_queue}`, `ConsistencyManager::queue_free()` in `X86PageTableImpl`, the page queues' `MarkAccessed`, `MSR_IA32_GS_BASE` in the NMI path; fbl's `AdoptRef`, which unlike ustl's `adopt()` does not retain; and OpenBSD's `mimmutable()` beside `mseal()`. The page queues' rotation defaults (10 s and 60 s) are Zircon's `kDefaultMinMruRotateTime`/`kDefaultMaxMruRotateTime`, which `page_queues.cpp` now points here for, and the free-list pointer near the middle of a free object is SLUB's placement, for the overrun and use-after-free reason the comment in `object_cache.cpp` now states.

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

## Title: kernel/object's comments say what each object does and why, without naming another kernel (2026-10-04)

**Files**
- `kernel/object/channel_dispatcher.cpp` — read()'s all-or-nothing rule and the peer-closed hook stand on their own reasons
- `kernel/object/dispatcher.cpp` — the destructor's observer check, rc_recycle()'s last-in first-out deferral and attach_observer()'s level trigger keep their reasons and drop their counterparts
- `kernel/object/exceptionate.cpp` — a failed send's task rights are overwritten by the next send, said as a fact
- `kernel/object/handle.cpp` — the arena's slot bookkeeping, the generation wrap, the Spectre V1 select, the destructor's cancel, the exhausted-arena warning and make()'s order keep their reasons; the counterpart paragraphs go
- `kernel/object/handle_table.cpp` — clean()'s one-at-a-time loop, unlink_locked()'s single cancel point, remove_for_transfer()'s order, duplicate()'s check order and lock span, and the port observer's allocation under the lock are explained in this table's terms
- `kernel/object/include/ours/object/channel_dispatcher.hpp` — RIGHTS_DEFAULT_CHANNEL says what it is; the pending-message limit says what a policy exception would wait for
- `kernel/object/include/ours/object/dispatcher.hpp` — attach_observer(), attach_port_observer(), PeerHolder, PeeredDispatcher's on_zero_handles() and init_peer() keep every reason and drop their counterparts; PeerHolder's deferred get_lock() step is stated as this tree's choice
- `kernel/object/include/ours/object/exception_dispatcher.hpp` — the default rights, what is left out, the five operations, and why m_response is a manual-reset Event
- `kernel/object/include/ours/object/exceptionate.hpp` — the class, its destructor and send()'s PeerClosed answer without the counterpart's names
- `kernel/object/include/ours/object/handle.hpp` — the index space, the arena size, HandleAccount, Handle and KernelHandle's operator=, destructor and two operator-> overloads
- `kernel/object/include/ours/object/handle_table.hpp` — the lookup order, clean(), get_dispatcher_with_rights(), attach_port_observer() and m_random
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp` — the default rights, the class, wait(), trigger(), destroy(), on_zero_handles(), Flags, the four hardware operations, State, begin_wait() and park()
- `kernel/object/include/ours/object/job_dispatcher.hpp` — RIGHTS_DEFAULT_JOB says what it is
- `kernel/object/include/ours/object/message_packet.hpp` — the handle array, the separate allocations and the kernel-buffer create()
- `kernel/object/include/ours/object/pager_dispatcher.hpp` — RIGHTS_DEFAULT_PAGER says what it is
- `kernel/object/include/ours/object/port_dispatcher.hpp` — the default rights, QueuedPacket, PortObserver, the port's semaphore, queue_user() and register_observer()
- `kernel/object/include/ours/object/process_dispatcher.hpp` — the default rights, start(), kill(), on_zero_handles(), exit(), get_info(), m_job, m_aspace and m_handle_table
- `kernel/object/include/ours/object/resource_dispatcher.hpp` — the default rights, create()'s System-only rule and validate_ranged_resource()
- `kernel/object/include/ours/object/thread_dispatcher.hpp` — the default rights, State's missing "created, not started" state, create(), kill(), on_bad_syscall(), on_user_exception(), deliver_exception(), on_policy_exception() and m_kernel_thread
- `kernel/object/include/ours/object/virtual_interrupt_dispatcher.hpp` — the class, the left-out untriggered signal, and create()'s option rule
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — the default rights, the class, create(), map()'s shape and answers, allocate()'s uses and answers, op_children_from_rights() and unmap()
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — the default rights, the class, create(), why the create_* helpers exist, create_from_wired_pages(), create_user(), size(), mark_vdso(), read_user(), write() and write_user()
- `kernel/object/include/ours/object/wait.hpp` — wait_one()'s closing sentence
- `kernel/object/interrupt_dispatcher.cpp` — wait(), begin_wait(), the Waiting answer, park(), trigger()'s Option and destroy()'s lock order
- `kernel/object/port_dispatcher.cpp` — the destructor, queue_user()'s check and post, dequeue()'s order, on_zero_handles(), queue_observer_packet() and the observer callbacks
- `kernel/object/process_dispatcher.cpp` — attach_thread(), on_zero_handles(), kill(), exit(), detach_thread(), finish_dead_transition() and get_info()
- `kernel/object/resource_dispatcher.cpp` — create()'s two System checks
- `kernel/object/thread_dispatcher.cpp` — the spawn counter, exit_current(), trampoline(), start()'s entry check, activate()'s `adopt`, on_exit(), on_user_exception(), deliver_exception(), on_policy_exception() and kill()
- `kernel/object/virtual_interrupt_dispatcher.cpp` — create()'s counterpart line goes; the header states the rule
- `kernel/object/vma_dispatcher.cpp` — create()'s rights, map()'s W^X, ceiling, vDSO and placement checks, and allocate()'s Perm, placement, allowance and CanMapSpecific rules
- `kernel/object/vmo_dispatcher.cpp` — create_user()'s order, create_child()'s rules and read_user()'s two easy-to-get-wrong answers
- `kernel/object/wait.cpp` — WaitSignalObserver and the stale-match answer
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
None; this carries out the user's decision 3 of 2026-10-04 evening, the sweep of rule 2 (CLAUDE.md: comments say what a thing is and why, in this codebase's own terms) over `kernel/object`. Before it, the sweep's grep found 206 comment lines in 32 of the directory's 38 files that offered Zircon as the reference for what the code does.

**Cause**
`kernel/object` was written by reading Zircon's object layer side by side, and its comments recorded that reading: almost every class and most functions carried a "Zircon counterpart: X" line, and many reasons were phrased as "as Zircon does". That is useful while porting and wrong once the code is ours, for four reasons, each of which this directory showed.

- **The comparison stood in for the definition.** `RIGHTS_DEFAULT_PAGER`'s whole comment was "Zircon ZX_DEFAULT_PAGER_RIGHTS.", and the other ten default-rights constants carried the same line. A reader who does not know Zircon learns nothing from it; one who does learns only where the value came from, not what a handle with those rights can do. The constant's comment has to say what it is (the rights a freshly created handle of that kind carries) and why each surprising choice is there (no Duplicate on a channel end, no Wait on a port or a region), and most of them already said the second half after the reference.
- **"As Zircon does" sounds like a reason and is not one.** `PortDispatcher::queue_user()` posted its semaphore "after the lock, as Zircon's QueueAndRemoveObserver() posts", and `Handle::make()` took "the account and the slot first, and only then the reference, in Zircon's order". In both the real reason followed: the woken waiter would only block on the lock if posted under it, and a refused make() must leave the KernelHandle holding the object so its destructor raises PEER_CLOSED. The foreign name added nothing a reader could check, and in the places where no reason followed it hid that one was missing. `ThreadDispatcher::on_user_exception()` said the process, not the thread, dies "as in Zircon"; the sentence after it is the actual reason (a faulted thread has left its process in a state nothing else in it can be trusted to recover from), and that is what a reader needs to defend the choice.
- **A comparison goes stale without anyone noticing, and hides it.** `ExceptionDispatcher::m_response` said it was manual-reset because "Zircon uses an AutounsignalEvent, which this tree's Event does not offer". That stopped being true when `sync::Event` gained `Options::AutoUnsignal` (InterruptDispatcher uses it, in the same directory), yet the comment still read as current, because its subject was the other kernel. The interrupt comments cited `interrupt_dispatcher.cc:65, :89` by line number, which no build checks. A comment about this tree's own invariant is checked every time someone reads it beside the code.
- **A "departure from Zircon" describes the other kernel, not this one.** `InterruptDispatcher::trigger()` explained its `ustl::Option<Instant>` as "where this departs from Zircon in one corner: Zircon keeps it in a zx_time_t whose 0 means none pending". The fact a reader needs is about ours: the time is kept in an Option because a bare time with 0 as "none pending" would lose a trigger at time 0 and leave the next wait parked. `VmoDispatcher`'s create_* helpers were "a deliberate departure from Zircon, where sys_vmo_create builds the VmObject"; the reason that matters is this tree's layering rule, that the system-call layer uses kernel/object and nothing below it.

Where a design here does follow Zircon, that is worth knowing, but it belongs where it is dated and attributed: this field's sibling, Reference, and the design documents. Reference below keeps every counterpart the comments named, so nothing a reader comparing the two kernels needs is lost.

**Change**
Only comment text changed; no token of code. Each hit was read in context and handled one of three ways: a "Zircon counterpart: X" line with nothing else in it was deleted; a sentence that gave a reason "as Zircon does" kept the reason and lost the clause; and a comparison that carried real information about ours was rewritten to state that information directly. The largest rewrites:

```cpp
// before (interrupt_dispatcher.cpp, trigger())
/// The time is pending until a wait takes it, which is where this departs
/// from Zircon in one corner. Zircon keeps it in a zx_time_t whose 0 means
/// "none pending", so a trigger at time 0 records nothing: in NeedAck it is
/// lost outright, and the next wait parks. An Option has no such value.

// after
/// The time is pending until a wait takes it, and it is kept in an Option
/// rather than in a bare time whose 0 means "none pending": with that, a
/// trigger at time 0 would record nothing, and in NeedAck it would be lost
/// outright, so the next wait would park. An Option has no such value.
```

```cpp
// before (exception_dispatcher.hpp, m_response)
/// discard_handle_close(): Zircon uses an AutounsignalEvent, which this
/// tree's Event does not offer, and with one handle out at a time the two
/// are the same.

// after
/// discard_handle_close(). With one handle out at a time that is the same
/// as an AutoUnsignal event, an option Event did not have when this was
/// written.
```

```cpp
// before (vmo_dispatcher.cpp, read_user())
/// Where the answers can differ from Zircon's:
///   - A buffer that cannot take the bytes is InvArgs, which is what
///     user_copy answers everywhere (design 4.6). Zircon says NotFound when
///     it is outside the user half or unmapped, and AccessDenied when it is
///     read-only.
///   - A physical VMO. Zircon's VmObject::ReadUser() answers NotSupported
///     before looking at the range or the buffer; here the range and user-half
///     checks come first and can answer OutOfRange or InvArgs instead. ...

// after
/// Two answers that are easy to get wrong:
///   - A buffer that cannot take the bytes is InvArgs, which is what
///     user_copy answers everywhere (design 4.6), whether it is outside the
///     user half, unmapped or read-only.
///   - A physical VMO is NotSupported, but only once the range and user-half
///     checks have passed: those come first and can answer OutOfRange or
///     InvArgs instead. ...
```

Others, in short:

- Eleven `RIGHTS_DEFAULT_*` constants: "Zircon ZX_DEFAULT_X_RIGHTS: ..." becomes "Rights a freshly created X handle carries: ...", keeping each reason; where the first line already said that (process, thread, VMA, VMO), the reference line is dropped.
- `ThreadDispatcher::State`: "Zircon's has Initialized and Suspended as well: it makes the core thread at creation and so needs Thread::Forget()" becomes "There is no state for 'created, with its kernel thread, not yet started', and no Suspended. The kernel thread is made at start, ... Making it at creation would need a way to dispose of a kernel thread that never ran, which this kernel does not have."
- `HandleTable::clean()`: "Zircon's HandleTable::Clean() swaps the whole list out ... One at a time here because ..." becomes "One at a time rather than swapping the whole list out under the lock and destroying it outside in one pass, because ...": the rejected alternative stays, described as a design, not as someone else's code.
- `HandleTable::unlink_locked()`: "Earlier than Zircon, which does not cancel in RemoveHandleLocked()" becomes the reason this is the one place waits are cancelled: a closed handle and a transferred one both leave through it.
- `ThreadDispatcher::on_bad_syscall()`: "Zircon's sys_invalid_syscall() calls Thread::Current::SignalPolicyException() directly; this is the same call, one layer up" becomes "It is on_policy_exception() with BadSyscall."
- `ThreadDispatcher::activate()`: "`adopt` reads as fbl::AdoptRef" becomes "`adopt` reads as 'take over a reference that already exists'", which is the trap the comment warns of, without the foreign library's name.
- `VmaDispatcher::map()` and `allocate()`: each "as in Zircon" after a check order (CanMapSpecific before the range, Perm bits refused on a region) goes; the order and its reason stay. "Zircon allows such a mapping and leaves W^X to policy; here it is the system's rule" becomes "W^X is a rule of the system here, not something left to policy."

The hits were found with the sweep's grep, `(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)`, over the C++, GN and Python files of `kernel/object`, and with a second grep for foreign identifiers the first cannot see (`zx_`, `ZX_`, `fbl::`, `.cc`, `.rs`, `.fidl`, "counterpart"), which found the `fbl::RefPtr` and `fbl::AdoptRef` mentions in `handle.hpp` and `thread_dispatcher.cpp`. Both greps are empty now.

**Reference**
The counterparts the comments named, kept here for the reader who wants to compare; all are Zircon's `zircon/kernel/object/` unless a path says otherwise.

- Default rights: `ZX_DEFAULT_CHANNEL_RIGHTS`, `ZX_DEFAULT_EXCEPTION_RIGHTS`, `ZX_DEFAULT_INTERRUPT_RIGHTS`, `ZX_DEFAULT_JOB_RIGHTS`, `ZX_DEFAULT_PAGER_RIGHTS`, `ZX_DEFAULT_PORT_RIGHTS`, `ZX_DEFAULT_PROCESS_RIGHTS`, `ZX_DEFAULT_RESOURCE_RIGHTS`, `ZX_DEFAULT_THREAD_RIGHTS`, `ZX_DEFAULT_VMAR_RIGHTS`, `ZX_DEFAULT_VMO_RIGHTS`.
- Dispatcher: `~Dispatcher()` asserts no observer is attached; `Dispatcher::fbl_recycle()` (`dispatcher.cc`) defers nested deletion through `SafeDeleter::Delete()` into `Thread::recursive_object_deletion_list()`, pushing and popping at the front; `Dispatcher::AddObserver()` is level-triggered; `MakePortObserver()` registers with the port under the object's lock before the level check; `PeerHolder`, whose lock Zircon also returns from each end's virtual `get_lock()`; `PeeredDispatcher::on_zero_handles()` centralised for every peered type; `InitPeer()`.
- Channel: `ChannelDispatcher` (`channel_dispatcher.cc`); its read leaves a message that does not fit at the front; `OnPeerZeroHandlesLocked()`; at its pending-message limit Zircon raises a policy exception.
- Handles: `Handle` and `HandleOwner` (`handle.cc`); `HandleTableArena` over `fbl::GPArena`, which keeps a freed slot's base value inside the slot (`PreserveSize`); `NewHandleValue()`, whose generation wraps through zero; `Handle::FromU32()`, which confines the slot address with `GPArena::Confine()` and selects with `fbl::conditional_select_nospec_eq()`; `HandleTableArena::Delete()` cancels waits; `Handle::Delete()` drops the handle count; a full arena logs throttled and returns null; `Handle::Make()` charges and takes the slot before the reference; a 2^18 index space, reserved and committed as used; Zircon bounds handles by its arena and job policy, not per process; `KernelHandle`'s move assignment and its destructor's `on_zero_handles()`; `fbl::RefPtr`'s shallow constness.
- Handle table: `HandleTable::Clean()` swaps the list out under the lock; `RemoveHandleLocked()` (`handle_table.cc`) does not cancel, so waits end as the handle is destroyed or as `msg_get_handles()` (`channel.cc`) takes it in; `get_handle_for_message_locked()` (`user_handles.cc`) removes first and checks after; `handle_dup_replace()` (`lib/syscalls/handle_ops.cc`) checks in the same order and holds the table lock across the lookup and the clone; `GetDispatcherWithRights()`; `sys_object_wait_async()` resolves both handles in one hold; `handle_table.cc` shifts its random XOR value by the reserved bits.
- Exceptions: `ExceptionDispatcher`, without the exception report, register context and second chance; `SetTaskRights()`, `MakeThreadHandle()`, `MakeProcessHandle()`, `WaitForHandleClose()`, `DiscardHandleClose()`, and an `AutounsignalEvent` for the response; `Exceptionate` and `~Exceptionate()`; `ZX_ERR_NEXT` for "no channel bound or its reader gone".
- Interrupts: `InterruptDispatcher` (`interrupt_dispatcher.cc`, `interrupt_dispatcher.h`): `WaitForInterrupt()`, `BeginWaitForInterrupt()` (BadState for a wait on an interrupt bound to a port; wake vectors; the event unsignalled under the lock at `:65` and `:89`), `DoWaitForInterruptBlock()`, `Trigger()` (a `zx_time_t` whose 0 means none pending), `Destroy()` (mask and unregister outside the lock, with the same L1/L2 reasoning in its comment), `on_zero_handles()` calling `Destroy()`, `Flags` and `InterruptState` with the same values and numbering, and the four pure virtuals; `zircon/vdso/interrupt.fidl`, `zx_interrupt_wait` (with no port bound, the next wait is the ack); `VirtualInterruptDispatcher` (`virtual_interrupt_dispatcher.cc`), whose `Create()` requires `ZX_INTERRUPT_VIRTUAL` and refuses other options, and which raises `ZX_VIRTUAL_INTERRUPT_UNTRIGGERED`.
- Message packets: `MessagePacket`, which holds its handles in an array it owns and chains fixed-size buffers for its 64 KiB limit; `MessagePacket::Create(const char *data, ...)`, the kernel-buffer overload.
- Ports: `PortDispatcher` (`port_dispatcher.cc`): `sema_`, `QueueUser()`, `MakeObserver()`, `~PortDispatcher()` only asserting, `QueuePacketLocked()` checking zero handles first, `QueueAndRemoveObserver()` posting after the lock, `Dequeue()` taking the unit first, `on_zero_handles()` tearing observers down with `CallUnlocked()` and not waking a sleeper; `PortObserver`, whose packet lives inside it, `PortObserver::dispatcher_`, `OnMatch()` taking `port_ref`, and `OnCancel()` through `MaybeReap()`.
- Processes: `ProcessDispatcher::Start()`, `Kill()` with `KillAllThreadsLocked()`, `on_zero_handles()`, `Exit()`, `GetInfo()`, `AddInitializedThread()` (which sets `start_time_`), `RemoveThread()` and `FinishDeadTransition()` (exceptionates first, then the address space, then `ShareableProcessState::DecrementShareCount()` and `HandleTable::Clean()`, and `ZX_TASK_TERMINATED` last); a job's list of processes, for killing and enumerating; `fbl::RefPtr<VmAspace>`; `create_shared()`, which gives two processes one handle table.
- Resources: `ResourceDispatcher::Create()`, which for `ZX_RSRC_KIND_SYSTEM` accepts only a size of 1 and a base inside the slot table, and keeps overlap checks in `Storage`; `validate_ranged_resource()` (`zircon/kernel/object/resource.rs`).
- Threads: `ThreadDispatcher` with its `Initialized` and `Suspended` states and `Thread::Forget()`; `Create()`, `Kill()`, `ExitCurrent()` calling `Thread::Current::Exit(0)`, `ExitingCurrent()` calling `RemoveThread()`, `HandleException()`, `MakeRunnable()` refusing a bad entry with `ZX_ERR_INVALID_ARGS`, `StartRoutine` calling `arch_disable_ints()` and `arch_iframe_process_pending_signals()` before `arch_enter_uspace()`, and `ResetCoreThread()`; the `dispatcher.thread.create` counter in the constructor; `sys_invalid_syscall()` calling `Thread::Current::SignalPolicyException()`; `dispatch_user_exception()`, which walks a debugger, the thread, the process and the job chain, prints the crash report only when no handler processed it, and kills with `ZX_TASK_RETCODE_EXCEPTION_KILL`; `fbl::AdoptRef` and `fbl::RefPtr<ThreadDispatcher>(this)`.
- Address regions: `VmAddressRegionDispatcher` (`Create()`, `Map()`, `Allocate()`, `Unmap()`, `op_children_from_rights()`); `VmAddressRegion::CreateVmMapping()` (`ZX_ERR_ACCESS_DENIED` for a Perm bit without its CanMap bit) and `CreateSubVmarInner()` (the vDSO check through `VDso::valid_code_mapping()` and `vdso_code_mapping_`, and an offset of 0 without Specific); `"vmar_flags & ~flags_ & VMAR_CAN_RWX_FLAGS"`; `zx_vmar_allocate`'s "It is NOT an error if the parent does not have ZX_VM_CAN_MAP_SPECIFIC permissions"; the `zx_vmo_create` / `zx_vmar_map` split; Zircon lets a mapping run past its VMO's end and faults the access, and leaves W^X to policy.
- VMOs: `VmObjectDispatcher` (`Create()`, `GetSize()`, `Read()`, `Write()`, `CreateChild()`, `parse_create_syscall_flags()`); `sys_vmo_create()` and `sys_vmo_create_child()`; `CreatePhysVmo()` (`zircon/kernel/top/handoff.cc`) with `VmObjectPaged::CreateFromWiredPages()`; `VmObject::size()` around `size_locked()`; the `VDso` class with its build-time `kVdsoCodeStart` and `kVdsoCodeSize`; `VmCowPages::CreateCloneLocked()`, bidirectional for an anonymous VMO; `VmObject::ReadUser()`, which answers NotSupported for a physical VMO first, and NotFound or AccessDenied for a bad buffer.
- Waits: `WaitSignalObserver` and `sys_object_wait_one()`, which also reports the signals asserted now after a stale match.

**Verification**
- `scripts/format_cpp.py --check` on all 32 C++ files: clean.
- No code changed: every changed diff line is a `//` comment line, and each of the 32 files, with its comments stripped and its whitespace collapsed, is the same before and after.
- Build and boot, `boot_check.py` on a fresh `out/bringup.x86`: build ok in 33.8 s; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required, 0 error lines).
- Kernel tests on the dbg image, `kshell_run.py`: `interrupt.test`, `port.test`, `vmaseal.test`, `vmawx.test`, `vmosnapshot.test` and `usercopy.test`, all passed.
- The two greps above, rerun over `kernel/object` after the change: no hit.
- clang-tidy with `-p out/bringup.x86` and `--header-filter=kernel/object/.*` on the directory's 17 sources: 16 distinct warnings, every one on a line of code this change did not touch (a missing `override` on `~SoloDispatcher()`, `~HandleTable()` and `~ResourceDispatcher()`, a missing `[[nodiscard]]` on `KernelHandle::dispatcher()`, and the case style of the GKTL_COUNTER and global object names), so none is new.

**Deliberately not done**
- Comments outside `kernel/object` that name Zircon about these objects stay for their own groups, among them `kernel/include/ours/task/thread.hpp`'s "Zircon counterpart: Thread::recursive_object_deletion_list()", which rc_recycle()'s comment now points at by its ours name.
- Paths into `.agents/tasks/` and design section numbers stay: they are this repository's own documents.
- `handle.hpp` still contrasts `ustl::Rc`'s deep constness with `std::shared_ptr`'s shallow one: that is the C++ standard library, not another system's design.
- No identifier changed, including those that keep a Zircon name (`PeerHolder`, `KernelHandle`, `rc_recycle`): rule 2 does not touch identifiers.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does InterruptDispatcher keep a pending trigger's time in an Option rather than a bare time, and what would a trigger at time 0 do to the next wait in NeedAck otherwise?
- Why does Handle::make() take the account and the slot before the reference to the object, and what does a refused make() owe a channel end's peer?
- HandleTable::unlink_locked() is the one place a handle's waits are cancelled; which two ways out of a table pass through it, and what would a wait that survived its handle leave behind?

## Title: the kernel's shared headers in kernel/include describe themselves without naming other systems (2026-10-04)

**Files**
- `kernel/include/ours/boot/handoff.hpp`, `boot/options.hpp` — take_boot_image() says what ends there (nothing in the kernel reads the image afterwards, and its pages belong to the VMO userboot is handed) instead of naming another kernel's handoff function; BootOptions and its options drop "the option of the same name, with the same default" and the comparisons with another phys stage and with other kernels' panic switches, and keep every fact: one parse, phys does not resolve `kernel.select.userboot`, the 64-byte copy, and why `kernel.name` here can disagree with phys's copy
- `kernel/include/ours/cpu.hpp`, `dpc.hpp`, `ipi_service.hpp` — the two constant masks, Dpc::arg(), SyncTask and sync_exec() describe themselves; sync_exec() now says why the request lives on the caller's stack (a call allocates nothing)
- `kernel/include/ours/irq/irq_object.hpp`, `irq/irq_observer.hpp` — IrqObject and IrqObserver say what they hold (the line's flags, hardware data and observer list; a handler function and its context) instead of naming two other kernels' structures; notify() holding L1 stays the stated reason detach() is a barrier
- `kernel/include/ours/sync/event.hpp`, `mutex.hpp`, `semaphore.hpp`, `spin_lock.hpp` — Event's flags, the mutex spin duration, assert_held(), the semaphore's never-negative count and its rejected atomic fast path, and the refusal of a policy-less `Guard<SpinLock>` are stated in this tree's terms; the spin lock's "three reasons" becomes two, because the third was only that another kernel does the same
- `kernel/include/ours/task/auto_preempt_disabler.hpp`, `cpu_stopper.hpp`, `exception.hpp`, `owned_wait_queue.hpp`, `preemption_state.hpp`, `thread.hpp`, `types.hpp`, `wait_queue.hpp` — the eager-reschedule relation, the stopper calls, the PI-lock-outside-queue-lock reason notify_threads() runs unlocked, the missing interrupt nesting count, the resource lock versus the PI lock, and the lock rules are each argued from this tree's own code; "counterpart" paragraphs that carried nothing else are removed
- `kernel/include/ours/task/sched/common.hpp`, `context.hpp`, `profiles.hpp`, `run_queue.hpp`, `sched_clock.hpp`, `sched_entity.hpp`, `schedule.hpp`, `scheduler.hpp`, `scheduler_inline.hpp`, `types.hpp` — the weight table, the base slice, SchedEntity, the run queue and its lock, charge_current()'s `elapsed <= 0` boundary, CpuAffinity's owned-mask-plus-pointer shape, init_first_thread() and the fixed-point types are described by what they are here
- `kernel/include/ours/time/cfg.hpp`, `clock.hpp`, `clock_event_device.hpp`, `clock_source.hpp`, `tick_device.hpp`, `tick_sched.hpp`, `timer.hpp`, `timer_queue.hpp` — HZ, the pure-map clock against the accumulating design, LateDeadline, the device and source ratings (now the values this tree's drivers use), the device selection rules, on_tick(), Timer's three pieces of state and TimerQueue's per-queue lock are stated without other kernels' function names; TimerQueue's class comment named `lock_queue_of()`, which does not exist, and now names `with_queue_locked()`, the function that does the job

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of these headers was sent to another kernel's source to learn what a lock rule, a constant or an object means here, and in a few places the comparison was the only reason given for a rule, so the reader could not judge whether the rule still held once the code around it changed.

**Cause**
The comment rule adopted on 2026-10-04 (CLAUDE.md: code comments say what a thing is and why, in this codebase's own terms) puts every comparison with another system in the change record's Reference field and in design documents, never in code. kernel/include holds the kernel's most-read headers -- the scheduler, the lock types, the thread, the timers -- and they were written while each mechanism was being rebuilt with another kernel's source open beside it, so the comments recorded where an idea came from as often as why it is right. That hides the real reason in three ways, and each needed a different rewrite.

First, the comparison was sometimes the whole justification. `RunQueue::m_rqlock` said it was "held with interrupts off by every holder, as Linux holds rq->lock and Zircon a scheduler's queue_lock_", and only then gave the reason that matters here: the tick takes it from interrupt context, so a holder the tick interrupted would spin on its own CPU's lock. The rewrite keeps the reason and drops the precedent. The precedent is no argument at all in this tree: the tree has its own rule (every spin lock held with interrupts off), and a lock that broke it would deadlock here whatever another kernel does.

Second, the comparison often carried the one piece of real information in a sentence. ClockEventDevice::Rating said the scale "is Linux's clock_event_device::rating, and so are the values the drivers use (PIT 110, local APIC 100, HPET 50 there)". The useful content is the values, and the quoted ones were not even this tree's: the HPET driver here rates its event device 120, not 50. The rewrite gives the values `kernel/driver/clock/i8253.cpp`, `kernel/arch/x86/apic/local_apic.cpp` and `kernel/arch/x86/kernel/time/hpet.cpp` actually use. In the same way, sync_exec()'s "this follows Zircon's shape, where the request lives on the caller's stack" became the property that shape buys -- a call allocates nothing -- which `kernel/kernel/task/ipi_service.cpp` relies on, because the first caller can run before there is a heap.

Third, a few comparisons stood in for a description of the object itself. IrqObject was "Linux's `struct irq_desc`, Zircon's `InterruptTableEntry`", which tells a reader who knows neither nothing; it is now "one interrupt line: its flags, its hardware data and the list of observers -- the handlers -- that its interrupts are dispatched to". A paragraph whose only content was the comparison ("Zircon counterpart: arch_exception_context_t ...") was removed whole, since nothing in it described this code.

Every rewritten claim was checked against the code it describes rather than carried over from the comparison: Thread::join() blocks with Interruptible::No (`kernel/kernel/task/thread.cpp`), irq::handle_irq_postamble() runs no deferred work (`kernel/kernel/irq/mod.cpp`), a straggling interrupt finds no handler because ClockEventDevice tests `m_event_handler` before calling it, Timer::activate() only forwards to TimerQueue::activate_timer(), and the latter ASSERTs (not DEBUG_ASSERTs) that the timer is not queued.

**Change**
Comment text only; no code, no identifier and no string changed. A script that strips C/C++ comments from the HEAD and working-tree copy of each of the 37 files and compares what is left reports no difference. Typical rewrites:

```cpp
// Before
/// One interrupt line: Linux's `struct irq_desc`, Zircon's
/// `InterruptTableEntry` (`zircon/kernel/platform/pc/interrupt_manager.rs`).
// After
/// One interrupt line: its flags, its hardware data and the list of
/// observers -- the handlers -- that its interrupts are dispatched to.

// Before
/// Locking. One spin lock per queue, taken with interrupts off, covers the
/// timer sets and the programmed deadline. Zircon uses a single global
/// TimerLock instead; a per-CPU lock is Linux's choice (hrtimer_cpu_base
/// ::lock), and the price of it is that a Timer must be able to find and
/// lock the queue it is on from another CPU -- lock_queue_of() does that,
/// the way Linux's lock_hrtimer_base() does.
// After
/// Locking. One spin lock per queue, taken with interrupts off, covers the
/// timer sets and the programmed deadline. The price of a lock per queue
/// rather than one for all of them is that a Timer must be able to find and
/// lock the queue it is on from another CPU; with_queue_locked() does that.

// Before
/// Held with interrupts off by every holder, as Linux holds p->pi_lock
/// (raw_spin_lock_irqsave): the tick's Thread::Current::preempt()
/// takes the running thread's from interrupt context, ...
// After
/// Held with interrupts off by every holder: the tick's
/// Thread::Current::preempt() takes the running thread's from interrupt
/// context, ...
```

Where a design choice was argued against another kernel's alternative, the alternative is kept and described by what it does, without whose it is: the semaphore's rejected fast path ("the other design lets wait() decrement the count below zero before it blocks"), the clock's rejected per-tick accumulation, and TimerQueue's rejected single global lock, whose trade-off (all CPUs serialise their timer operations on one lock) stays in the comment because it is the reason for the choice.

**Reference**
The comparisons these comments used to carry, recorded here so they are not lost now that the code no longer names them. Zircon (`zircon/kernel/` in the Fuchsia tree): `EndHandoff()` (`top/handoff.cc`); `BootOptions` and `gBootOptions` (`lib/boot-options`, the options `kernel.select.userboot`, `kernel.shell`, `kernel.select` and `kernel.halt-on-panic` in `options.inc`) and physboot's `HandoffPrep::UsePackageFiles()`; `Dpc::arg<T>()`; `mp_sync_exec()` and `mp_sync_task_t`; `InterruptTableEntry` and `invoke_if_present()` (`platform/pc/interrupt_manager.rs`) and `interrupt_handler_t`; `Event::Flags` and `AUTOUNSIGNAL`, `AutounsignalEvent` and `InterruptDispatcher::event_`; `TaskState::Join()`; `Mutex::SPIN_MAX_DURATION` and `Mutex::AssertHeld()`; `Semaphore` (`include/kernel/semaphore.h`) and `Semaphore::Post()`; the `Monitored`/`Unmonitored` spin lock split and `LOCK_DEP_POLICY_OPTION`; `EagerReschedReenable`; `try_dispatch_user_exception()`, `arch_exception_context_t` and `dispatch_user_exception()`; `OwnedWaitQueue::WakeThreadsLocked()` and the ChainLock transaction; `PreemptionState::PreemptIsEnabled()`, `PreemptReenableDelayFlush()`, `int_handler_start()`, `int_handler_saved_state_t` and `x86_nmi_handler()`; `Thread::Join()`, `Thread::Detach()`, `ConstructFirstThread()`, `Thread::recursive_object_deletion_list()`, `signals_`, `flags_` and `task_state_` under `thread_lock`, and the `thread_list` lock in `Thread::CreateEtc()`; `thread_start_routine` and `THREAD_SIGNAL_*`; `WaitQueueBase::UnblockThread()` and `Event::WaitWorker()`; the scheduler's weight formula, `SchedWeight`, `SchedDuration` and `SchedTime` as `ffl::Fixed`, `BaseProfile(int)` with `SchedulerState::ConvertPriorityToWeight()`, `SchedulerState`, `UpdateTimeline()`, `Scheduler::InitializeFirstThread()`, `Scheduler::Unblock()`, `FinalContextSwitch` and `active_thread_`; `Timer` (`kernel/timer.cc`), `TimerSlack`, `Timer::Set()`, `Timer::Cancel()`, `Timer::TrylockOrCancel()`, `TimerQueue` and the global `TimerLock`; `zx_ticks_t` and `current_mono_time()`. Linux (`kernel/` in the Linux tree): `cpu_none_mask` and `cpu_all_mask`; `smp_call_function_many()`; `struct irq_desc` and `struct irqaction`; `struct semaphore` and `up()` (`kernel/locking/semaphore.c`); `stop_one_cpu()`, `stop_cpus()`, `stop_one_cpu_nowait()` and `struct cpu_stopper`; `preempt_count`'s HARDIRQ and NMI bits, `irq_enter_rcu()`, `__irq_exit_rcu()`, `irqentry_enter()` and `raw_irqentry_exit_cond_resched()`; `alloc_lock` and `task_lock()`, `p->pi_lock`, `pi_waiters`, `pi_blocked_on` and `tasklist_lock`; `struct rq`, `cfs_rq`, `sched_class`, `sysctl_sched_base_slice`, `update_curr_common()`, `yield_to_task_fair()`, `pick_task()` and `put_prev_set_next_task()`, `rq->clock`, `update_rq_clock()` and `hrtick_start()`, `struct sched_entity` and `task_of()`, `task_struct::cpus_ptr` and `cpus_mask`, `lockdep_assert_rq_held()`; `HZ`; the timekeeper's `cycle_last` and `xtime_nsec`; `clockevents_program_event()`, `clock_event_device::rating`, `clockevents_config_and_register()` and `clockevents_exchange_device()`; `clocksource::rating`, `clocksource_watchdog`, `clocksource_jiffies` and `tick_do_timer_cpu`; `tick_check_new_device()`, `tick_check_percpu()`, `tick_check_preferred()`, `tick_setup_device()`, `tick_setup_periodic()`, `tick_setup_oneshot()` and `tick_switch_to_oneshot()` (`kernel/time/tick-common.c`, `tick-oneshot.c`); `update_process_times()`, `run_local_timers()` and `tick_sched_timer`; `hrtimer_cpu_base`, `hrtimer_clock_base`, `lock_hrtimer_base()`, `hrtimer_forward()` and `hrtimer_cpu_base::running`; the `panic=` option.

**Verification**
`scripts/format_cpp.py --check` clean on all 37 headers. clang-tidy (`-p out/bringup.x86`) over `kernel/kernel/time/timer_queue.cpp`, `tick_device.cpp` and `tick_sched.cpp` with a header filter on the edited time headers reports only the existing `readability-identifier-naming` warnings on static members of `clock_source.hpp`, none on a changed line. boot_check: build ok, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required). The grep below, over kernel/include, finds only the four hits listed under "Deliberately not done".

**Deliberately not done**
Four hits stay. `kernel/include/ours/sampler/mod.hpp` lines 15, 16 and 23 compare the sampler with another kernel's thread sampler and its minimum period; another session is changing that file, so this sweep does not touch it. `kernel/include/ours/task/sched/fair.hpp` line 20 compares the run queue's embedding of the Fair state with another kernel's run queue; the Fair discipline (`fair.hpp`, `fair.cpp`) belongs to the user, who takes no edits there of any kind, mechanical ones included, so that comment is theirs to rewrite. Comments outside kernel/include, including the `.cpp` files that implement these headers, are other groups' sweeps. No identifier was renamed, and no comment was changed for any reason but rule 2, apart from the one stale function name (`lock_queue_of()`) inside a sentence that had to be rewritten anyway.

**How it was found**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over kernel/include, which holds only C++ headers, then each hit read in context together with its neighbouring lines, and a second grep for foreign artefacts named without the system's name (`hrtimer`, `tick_*()`, `clockevents_`, `task_struct`, `cfs_rq`, `sched_class`, `preempt_count`, `thread_lock`, `ZX_`, `.cc` paths, "counterpart", "mirrors", "-style").

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is "the tick takes this lock from interrupt context" a better reason for holding the run-queue lock with interrupts off than "another kernel holds its run-queue lock that way"?
2. When a comment justified a choice by another kernel's alternative (a single global timer lock, a semaphore count that goes negative), what must the rewritten comment still say once the other kernel's name is gone?
3. The rating comment quoted HPET at 50; what does this tree's HPET driver actually use, and what does that say about copying another system's numbers into a comment?
