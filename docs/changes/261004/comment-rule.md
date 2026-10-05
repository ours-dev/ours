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

## Title: review fixes to the kernel/include sweep: an unattributed rating scale, a dropped condition, three foreign method names (2026-10-04)

**Files**
- `kernel/include/ours/time/clock_source.hpp` — ClockSource::Rating gives the value this tree's one enrolled source uses (the HPET, 250) and what the gap around it is for, instead of a scale this tree does not have
- `kernel/include/ours/task/owned_wait_queue.hpp` — notify_threads() says again under what condition it could wake under the queue lock
- `kernel/include/ours/task/thread.hpp` — spawn()'s comment names Current::exit(), join() and detach() instead of Exit(), Join() and Detach()
- `docs/repairs/261004/comment-rule.md` — new: the two hits the sweep had to leave in kernel/include, each claimable

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of ClockSource::Rating was told jiffies rates 1, an ACPI PM timer 200 and a TSC 300, and could find none of the three in the tree; a reader of notify_threads() could no longer tell what would let it run under the queue lock; a reader of Thread::spawn() was sent to three methods that do not exist under those names.

**Cause**
The sweep's own record names the danger of copying another system's numbers into a comment, and fixed it for ClockEventDevice::Rating, but ClockSource::Rating kept the same kind of numbers and only lost their attribution. Before, "on Linux's clocksource::rating scale (jiffies 1, ACPI PM timer 200, HPET 250, TSC 300)" was at least honest about whose scale it was. After, "on a scale where jiffies is 1, an ACPI PM timer 200 ..." stated it as a fact about this tree, where it is false: the jiffies source never enrolls (ClockSource::Default::init() has no definition) and so has no rating at all, and there is no ACPI PM timer or TSC clock source. The one true number is the HPET's 250 (`HPET_CLKSRC_RATING` in `kernel/arch/x86/kernel/time/hpet.cpp`), and what the gap around it is for.

notify_threads()'s old comment had two halves: why it cannot wake under the queue lock here (the PI lock is the outer lock), and what would change that (another design wakes under it only because one transaction already holds every lock in the chain). The sweep removed the second half with the other kernel's name, but it was a condition on this tree's design, not a comparison: the reserved `lock_pi_chain_*` functions are where a whole-chain locking protocol would go, and with it the wake could move under the lock.

Thread::spawn()'s comment was copied from another kernel's thread header and still used its method names. They are not offered as a comparison, so the sweep's grep for "counterpart" and the like did not find them, but they are another kernel's names all the same, and here they name nothing. A check that lists every `name()` in kernel/include's comments that no real code declares found them, along with two in-tree names that are not about rule 2 (`finish_switch_context()` in scheduler.hpp, which no code defines, and `kernel_elf_image()` in options.hpp, a GN template).

The two hits the sweep had to leave were recorded only in the change record's "Deliberately not done". A change record is history; what is still to be done goes in `docs/repairs`, and for the Fair discipline that is the only route `ours-kernel-design` allows.

**Change**
Comment text only, plus the new repairs file.

```cpp
// Before
/// How good a time source this is, higher being better, on a scale where
/// jiffies is 1, an ACPI PM timer 200, the HPET 250 and a TSC 300.
// After
/// How good a time source this is, higher being better. The HPET, the
/// only source enrolled today, rates itself 250, which leaves room above
/// it for a better counter (a TSC) and below it for a worse one (an ACPI
/// PM timer).

// Added after "Hence no "_locked" suffix." on notify_threads()
/// Waking under the queue lock becomes possible only once every lock the
/// wake needs is already held before it starts, taken up front for the
/// whole chain.

// Before: passed to Exit(). ... freed by either Join() or Detach()
// After:  passed to Current::exit(). ... freed by either join() or detach()
```

**Reference**
Linux `clocksource::rating` and the ratings of `clocksource_jiffies` (1), `clocksource_acpi_pm` (200), the HPET clocksource (250) and the TSC clocksource (300), which ClockSource::Rating used to quote. Zircon `OwnedWaitQueue::WakeThreadsLocked()` under a ChainLock transaction, the design that wakes under the queue lock. Zircon `Thread::Create()`, `Thread::Current::Exit()`, `Thread::Join()` and `Thread::Detach()` in `zircon/kernel/include/kernel/thread.h`, whose comment spawn()'s was copied from.

**Verification**
A comment-stripped comparison of every changed header against `mvp/integration` reports no code difference. `scripts/format_cpp.py --check` is clean on the three headers. clang-tidy (`-p out/bringup.x86`) over `kernel/kernel/time/clock_source.cpp` and `kernel/kernel/task/owned_wait_queue.cpp`, filtered to the three headers, reports only existing `readability-identifier-naming` and `modernize-use-nullptr` warnings, none on a changed line. boot_check: build ok; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required).

**Deliberately not done**
`finish_switch_context()` (scheduler.hpp) names a function nothing defines; it is a stale in-tree name, not a rule-2 hit, so it is left for its own fix. `kernel/include/ours/sampler/mod.hpp` and `kernel/include/ours/task/sched/fair.hpp` are still not touched; `docs/repairs/261004/comment-rule.md` items 1 and 2 hold them.

**How it was found**
Review of the sweep: each rewritten claim was checked against the code, and the rating values against `ClockSource::enroll()` callers (only `hpet.cpp` calls it). A script listed every `name()` in kernel/include's comments that no comment-stripped source file in kernel/ or system/ declares.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is a number copied from another system worse once its attribution is removed than while it still carried it?
2. What would have to be true of the locking protocol for notify_threads() to wake its threads while still holding the queue lock?
3. Why does a remaining rule-2 hit in the Fair files go into docs/repairs rather than only into a change record?

## Title: kernel/lib's comments say what a thing is and why, without naming another system (2026-10-04)

**Files**
- `kernel/lib/syscall/src/channel.cpp`, `exception.cpp`, `handle.cpp`, `interrupt.cpp`, `object.cpp`, `port.cpp`, `process.cpp`, `table.cpp`, `task.cpp`, `vma.cpp`, `vmo.cpp`, `argument.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — each handler's "Zircon: sys_*()" header becomes a line saying what the call does, and each "in its order" list of checks says "In order" in its own right; comparisons that were the only stated reason ("as Zircon does", "Zircon answers ZX_ERR_BAD_SYSCALL here") are replaced by the reason itself or dropped where the sentence before already gave it
- `kernel/lib/syscall/test/fault_probe/BUILD.gn` — the test's description loses the pointer to another kernel's core test
- `kernel/lib/heap/BUILD.gn`, `kernel/lib/heap/include/heap/new.hpp`, `kernel/lib/heap/include/heap/vmalloc.hpp`, `kernel/lib/heap/src/new.cpp`, `kernel/lib/ktl/include/ktl/memory_resource.hpp` — the kmalloc/vmalloc split, the half-open window and the routing in `operator delete` are explained without naming the allocator functions they resemble; the warning about an inclusive end tested with `<` is kept as a statement about this window
- `kernel/lib/user_copy/BUILD.gn`, `kernel/lib/user_copy/include/user_copy/copy_allowed.hpp`, `range.hpp`, `user_copy.hpp`, `kernel/lib/user_copy/test/user_copy_test.cpp` — why `CopyAllowed` asks for trivially copyable rather than `std::is_trivial`, and why a zero-length range must still start inside, now stand on their own
- `kernel/lib/userabi/include/userabi/userboot.hpp`, `kernel/lib/userabi/src/resource.hpp`, `kernel/lib/userabi/src/userboot.cpp`, `kernel/lib/userabi/userboot/BUILD.gn`, `kernel/lib/userabi/userboot/userboot.cpp`, `kernel/lib/userabi/vdso/BUILD.gn`, `kernel/lib/userabi/vdso/vdso.S` — userboot's option forwarding, the boot image VMO, the bootstrap channel, the vDSO's single VMO and the stub shape are described by what they do here
- `kernel/lib/arch/include/arch/cache.hpp`, `nospec.hpp`, `paging/mod.hpp`, `kernel/lib/crashlog/include/crashlog/crashlog.hpp`, `kernel/lib/kconcurrent/BUILD.gn`, `kernel/lib/kconcurrent/include/kconcurrent/chain_lock_transaction.hpp`, `kernel/lib/kelfload/include/kelfload/loader.hpp`, `kernel/lib/kshell/kshell/shell.cpp`, `kernel/lib/logz4/kernel.cpp`, `kernel/lib/obi/include/obi/command_line_view.hpp`, `kernel/lib/obi/include/obi/checksum.hpp`, `kernel/lib/gktl/include/gktl/defer.hpp` — one comparison each, removed or rewritten; `checksum.hpp` now says which digest fields the CRC fills and what the zeroed range is for, and `defer.hpp` names the two rules that hold an action to one run

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of these comments was sent to another code base, often to a named source file in it, to learn why a check comes first or why a value is refused here, and the reason itself was frequently not written down at all: "the handle first, then the packet, as Zircon's sys_port_queue() does" says whose order it is, not why a bad handle should outrank a bad pointer.

**Cause**
The comment rule adopted on 2026-10-04 (CLAUDE.md: code comments say what a thing is and why, in this codebase's own terms) moves every comparison with another system into the change record's Reference field and the design documents. kernel/lib's comments leaned on comparison in three ways, each of which hides something.

First, a comparison stood in for the reason. "BadSyscall, as Zircon's sys_invalid_syscall() answers" tells the reader the number agrees with someone else's, but the property that makes BadSyscall right is that a program must be able to tell "that was not a system call" from "your handle cannot do that", which the comment already said in the sentence before; the comparison added only a citation, so it went. Where the comparison was the only reason given, the reason was written in its place: `vmo_replace_as_executable` used to say the invalid vmex handle is refused "(Zircon asks the job policy, and ours has no ambient Execute to allow)", and now says there is no ambient Execute to allow without the resource.

Second, a comparison often carried a fact about this code inside a sentence about another. "Zircon's RemoveMapping does [free tables], deferring the free past the flush" contained the real reason this kernel's unmapping never frees a page table: a freed table would have to wait out the TLB flush that retires it. "x86 Linux's VMALLOC_END is the last valid address while is_vmalloc_addr() compares with `<`, a mismatch worth not copying" contained a rule for this window: no constant names the last valid address, because an inclusive end compared with `<` loses the window's last byte. "Zircon hands userboot the ZBI the same way, a paged VMO ... not a physical VMO" contained the decision that the boot image goes to userboot as a paged VMO, and the design it cites gives the reasons (a physical VMO never frees its frames, and vmo_read refuses one); the rewrite states both. Dropping such a sentence whole would have lost the rule, so each one was rewritten to keep it.

Third, a handler's doc comment was often nothing but a pointer ("Zircon: sys_port_create() (kernel/lib/syscalls/port.cc)."), which left the function undocumented once the pointer is gone. Those now say what the call does in one line ("A new port, and a handle to it.").

**Change**
Comment text only; no code, no identifier and no string a program reads changed. A check that every changed line of the diff is a comment line passes. Typical rewrites:

```cpp
// Before
/// paging-structure cache. Unmapping never frees a table (Zircon's
/// RemoveMapping does, deferring the free past the flush), so an address
// After
/// paging-structure cache. Unmapping never frees a table (a table freed
/// there would have to wait out the flush that retires it), so an address

// Before
/// The handle first, then the packet, as Zircon's sys_port_queue() does: a
/// bad handle outranks a bad pointer (design 4.5).
// After
/// The handle first, then the packet: a bad handle outranks a bad pointer
/// (design 4.5).

// Before
// CRC-32/ISO-HDLC, matching the checksum used by ZBI-style images.
// After
// CRC-32/ISO-HDLC, the common reflected CRC-32 (polynomial 0xEDB88320): the
// digest an OBI container keeps for each item's payload, and for itself, when
// ContainerFlags::Crc32 is set. Bytes in [zero_offset, zero_offset +
// zero_size) are read as zero, so that the container's own digest can be
// computed over a header that holds it.
```

Left as they are, because rule 2 does not reach them:
- `kernel/lib/arch/include/arch/asm.hpp:22`, "The following asm macros is from Zircon kernel." — an attribution of copied code, which the rule leaves alone.
- `kernel/lib/code_patching/hermetic_stub.py:2`, "Copyright 2021 The Fuchsia Authors" — another project's copyright header.
- `kernel/lib/unwind/include/unwind/cfi.hpp:19`, "LSB 4.1 "DWARF Extensions"" — the specification that defines the `DW_EH_PE_*` pointer encodings, a fact about the ELF format, not a design comparison.
- `kernel/lib/kshell/README.md:33` — a document, and another session's file.
- `kernel/lib/prof/include/prof/sampler.hpp:162` — under `kernel/lib/prof`, which another session is changing.

**Reference**
What the removed comments compared against, kept here so the comparison is not lost.
- Zircon syscalls (`zircon/kernel/lib/syscalls/`): `sys_channel_write()`/`RemoveUserHandles()`, `sys_channel_read()`, `msg_get_handles()`; `sys_task_create_exception_channel()`, `kExceptionChannelRights`, `sys_exception_get_thread()`, `sys_exception_get_process()` (`exceptions.cc`); `zx_handle_close()`; `sys_interrupt_create()`, `sys_interrupt_wait()`, `sys_interrupt_trigger()`, `sys_interrupt_destroy()` (`driver.cc`); `sys_object_wait_async()` (`object_wait.cc`), `sys_object_get_info()`, `single_record_result()`, `actual_avail_result()` (`object_info.rs`), `sys_object_set_property()` with `ExceptionDispatcher::SetDisposition()` (`object.cc`); `sys_port_create()`, `sys_port_queue()`, `sys_port_wait()` (`port.cc`); `sys_process_exit()` with `ProcessDispatcher::ExitCurrent()`, `sys_process_create()` and `ZX_MAX_NAME_LEN`, `sys_process_start()`; `sys_thread_create()`; `sys_vmar_allocate()`, `sys_vmar_map()`, `sys_vmar_unmap()` and `op_children_from_rights()`; `sys_vmo_create()`, `sys_vmo_read()`, `sys_vmo_write()`, `sys_vmo_create_child()` with `ZX_VMO_CHILD_NO_WRITE`, `sys_vmo_replace_as_executable()`, `sys_vmo_get_size()`; `SafeSyscallArgument<T>::Sanitize()` (`include/lib/syscalls/safe-syscall-argument.h`) and the generated `kernel-wrappers.inc`, whose copy-out failure raises a HANDLE_LEAK policy exception.
- Zircon syscall entry: `VDso::ValidSyscallPC()` and the `CODE_SYSRET_*` labels, `sys_invalid_syscall()` with `SignalPolicyException()` and `ZX_EXCP_POLICY_CODE_BAD_SYSCALL`, `unknown_syscall`, `do_syscall_pre()`/`do_syscall_post()`, the `jae`/`cmovge` pair in x86 `syscall.S`; the vDSO stub's register plan and offsets in `m_syscall` (`kernel/lib/userabi/vdso/syscalls-x86.S`); `VDso::Create()` (`kernel/lib/userabi/vdso.cc`); verify-abi as a public_dep of the vDSO; programs calling `zx_*` against `libzircon.so`; Fuchsia's toolchain passing `-z rodynamic` for all user code; the `bad-syscall` core test (`zircon/system/utest/core/bad-syscall`).
- Zircon userboot (`zircon/kernel/lib/userabi/userboot.cc`, `userboot/main.rs`): `crashlog_to_vmo()`, `crashlog_stash()`, `get_resource_handle()`, `compute_initial_stack_pointer()`, `BootstrapChannel::Create()`, `parse_option()`, `get_options()`, `launch_program()`, `get_bootfs_vmo()` (`src/bringup/lib/userboot/rust/src/bootfs.rs`), the bare "userboot" in the STORAGE_KERNEL BootFS, `zircon.autorun.boot` served as `fuchsia.boot.Arguments`; the ZBI handed over as a paged VMO by `CreatePhysVmo()`/`VmObjectPaged::CreateFromWiredPages()` (`zircon/kernel/top/handoff.cc`); process_builder's order.
- Zircon libraries: `arch_clean_cache_range()`; `fbl::confine_array_index()` and `fbl::conditional_select_nospec_eq()`; `RemoveMapping` in the x86 page tables; ram-crashlog (`zircon/system/ulib/ram-crashlog`), which keeps two header copies because it updates uptime in place; the `system/ulib/concurrent` and `kernel/lib/kconcurrent` split and `ChainLockTransaction`'s percpu slot; `kernel_shell_init()` and `gBootOptions->shell`; `BootOptions::Parse(bool)`; `user_ptr<T, Policy>`, `is_copy_allowed` (`lib/user_copy/internal.h`), `is_user_accessible_range()`, `UserCopyCaptureFaultsResult`, the `padding1` fields of the info structs; the ZBI's CRC-32; `fit::deferred_action` (`sdk/lib/fit/include/lib/fit/defer.h`).
- Linux: the slab/vmalloc split (`mm/slub.c`, `mm/vmalloc.c`), `kvmalloc()`/`kvfree()`, `is_vmalloc_addr()`, `VMALLOC_START`/`VMALLOC_END` (inclusive on x86), `vmalloc()`/`__vmalloc_node_range()`, `vfree()`; `serial8250_console_write()`, which only try-locks the port while `oops_in_progress` is set.
- Where the removed comments recorded a deliberate departure, kept so that it is not "fixed" back to the reference: `sys_channel_write()` reads an over-limit handle array in chunks and consumes it, where `channel_write` refuses the count before reading anything and consumes nothing; `sys_vmar_map()` allows Write with Execute, and execute-only, where `vma_map` enforces W^X and has both need Read (D3); `sys_vmo_replace_as_executable()` asks the job policy about an invalid vmex and keeps Write for JITs, where `vmo_replace_as_executable` answers AccessDenied and drops Write (D3); `sys_object_set_property()` answers an unknown property with InvArgs after the handle lookup, `object_set_property` before it; `BootOptions::Parse(bool)` reads anything but `false`/`0`/`off` as true, where `CommandLineView::parse_bool()` reports an unknown word; `BootstrapChannel::Create()` `release()`s the kernel's end so Zircon's userboot never sees PEER_CLOSED, where this kernel drops its `KernelHandle` and the process sees PEER_CLOSED beside the message; Zircon's userboot passes its crashlog VMO on to the next program, where ours reports the first line and closes it; Zircon's userboot reads its options from the ZBI's CMDLINE item and hands the ZBI on, where this kernel forwards the `userboot.*` and `ours.autorun.*` tokens as processargs environment strings; `VDso::ValidSyscallPC()` checks the exact `CODE_SYSRET_*` set, where `trapped_from_vdso()` checks the vDSO's range; and `arch::confine_array_index()` and `arch::conditional_select_nospec_eq()` follow the `fbl` versions instruction for instruction.

**Verification**
`scripts/format_cpp.py --check` and `scripts/format_gn.py --check` clean on every changed file; clang-tidy on `kernel/lib/syscall/src/vmo.cpp`, `exception.cpp` and `kernel/lib/heap/src/new.cpp` reports only pre-existing diagnostics in headers this change does not touch. boot_check: build ok, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required). Host tests: crashlog 9/9, obi container-view-test 4/4, obi package-builder-test 5/5, user_copy 6/6. A grep of `kernel/lib` for `zircon|fuchsia|linux|\blk\b` in comments finds only four of the five lines listed above; the fifth, `cfi.hpp`, came from the second pass. A review pass (2026-10-04) checked that the diff from `mvp/integration` changes no code: with comments and whitespace stripped, each of the 43 changed source and build files is identical to its base. It also tightened three of the new one-line summaries in `handle.cpp`, `interrupt.cpp` and `exception.cpp`, which overstated or left out part of what the code does. A second review pass (2026-10-05) repeated the stripped-comparison check and fixed two rewrites that had turned a statement about the reference kernel into a claim about this one: `names_a_job()` said exception channels "bind to jobs as well", which this kernel does not do (it answers NotSupported), and `copy_allowed.hpp`'s padding example used the reference's `uint8_t` where this tree writes `u8`.

**Deliberately not done**
No identifier was renamed, even where it is borrowed (`VmCowPages`, `x86_syscall_process_pending_signals`): identifiers are outside the rule. `kernel/lib/prof` and `kernel/lib/kshell/README.md` belong to another session's work in progress. Comments in `kernel/object`, `kernel/arch` and the rest of the tree are other groups' sweeps, including the doc comments of `ExceptionDispatcher` and `InterruptDispatcher` that the rewritten syscall comments now paraphrase.

**How it was found**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over the C, C++, assembly, GN and Python files of `kernel/lib`, each hit read in context; then a second pass over the same files for comparisons that do not name the system (`ZBI`, `fit::`, `fbl::`, `sys_*()`, `.cc` and `.rs` paths, "counterpart", "modelled on", "mirrors"), which found `checksum.hpp` and `defer.hpp`. Each rewritten claim was checked against the code it describes (for example, that `make_process_handle()` refuses a thread-level channel with AccessDenied, and that `.agents/tasks/boot-image-pages/DESIGN.md` rejects a physical VMO because it never frees its frames and refuses vmo_read).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does this kernel's unmapping never free a page table, and what would freeing one during unmap require first?
2. Why does `vmo_replace_as_executable` drop Write from the new handle, and who pays for that choice?
3. When a comment's only content was "the same order as another kernel's handler", what has to be written in its place for the comment to still be worth keeping?

## Title: the comments in kernel/arch and kernel/phys say what a thing is and why, without naming another system (2026-10-04)

**Files**
- `kernel/arch/BUILD.gn` — `:ffi` and `:arch-rs` are described as the architecture code's Rust seam and a stand-in for a library that does not exist yet
- `kernel/arch/riscv/hypervisor/BUILD.gn` — says there is no RISC-V hypervisor yet, so the target is an empty group
- `kernel/arch/riscv/lib/arch/include/arch/riscv/interrupt.hpp` — `ArchCpuState` is a bool because SIE is the only bit touched, set against x86 and arm64 in this tree
- `kernel/arch/x86/BUILD.gn` — the retpoline config's TODO says what is missing (no rustflags) instead of linking another project's bug tracker
- `kernel/arch/x86/apic/io_apic.cpp` — an IOAPIC without an EOI register needs the edge-and-back toggle, which is not implemented; one bad MADT override is refused and the rest kept
- `kernel/arch/x86/apic/local_apic.cpp` — the timer rating, the per-CPU handler, the jiffy-aligned window, the 200 ms span, the 10% warning and the 0xF/0x7FFFFFFF bounds keep their reasons and lose their comparisons
- `kernel/arch/x86/kernel/bootstrap16.S` — rva() is explained by what it does; the null return address keeps its reason
- `kernel/arch/x86/kernel/descriptor.cpp` — the IST limits and the read-only GDT alias are explained in this kernel's terms
- `kernel/arch/x86/kernel/exception.S` — `paranoid_common`'s comparison paragraph is dropped
- `kernel/arch/x86/kernel/idt.S` — the CLAC's dependent is named as `idt.cpp`, not `idt.c`, and the instruction that restores AC as `iretq`, the one the stubs return with, not the 32-bit `iretd`
- `kernel/arch/x86/kernel/idt.cpp` — why the gates skip the CLAC rather than patching it, and why #DB keeps the thread's stack
- `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp` — the vmalloc window, the 2 MiB user base, the missing top page and the restricted size, without comparisons
- `kernel/arch/x86/kernel/include/ours/arch/entry.hpp` — why entry 0 is refused, why interrupts must be off, why IOPL is 0
- `kernel/arch/x86/kernel/include/ours/arch/thread.hpp` — two comparison paragraphs dropped
- `kernel/arch/x86/kernel/include/ours/arch/time.hpp` — `global_clock_event()` no longer named after a foreign variable
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — why the SMAP choice is a cached flag; why `UserCopyCaptureResult` is part of the arch interface
- `kernel/arch/x86/kernel/include/ours/arch/x86/descriptor.hpp` — the user selectors are stated as values, not as a foreign macro's
- `kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp` — `X86Tss::init()` names its own caller; the `(%rip)` reason stands alone
- `kernel/arch/x86/kernel/init.cpp` — the RFLAGS mask's comparison paragraph is dropped
- `kernel/arch/x86/kernel/mp.cpp` — the resched IPI's EOI rule and the AP start-up deadline keep their reasons
- `kernel/arch/x86/kernel/syscall.S` — the saved-stack slot, the unconditional signal check, `popq %rsp` and the SYSRET argument keep their reasons
- `kernel/arch/x86/kernel/task/idle_thread.cpp` — the sti;hlt comparison is dropped
- `kernel/arch/x86/kernel/time/hpet.cpp` — the two ratings, the comparator verdict, the double write, the counter check and the period bounds state their own reasons
- `kernel/arch/x86/kernel/time/time.cpp` — the legacy timer line is no longer named after a foreign variable
- `kernel/arch/x86/kernel/user_copy.S` — why there is one quadword copy and a cached flag; comparison paragraphs dropped
- `kernel/arch/x86/kernel/user_copy.cpp` — the bit-63 check, the lfence, the AC assertion and InvArgs keep their reasons
- `kernel/arch/x86/lib/arch/include/arch/paging/x86_pagings.hpp` — Global and the table-entry permissions are argued from this layer alone
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — SVR, the bounded and unbounded ICR waits, and the one interrupt-off window for a set of IPIs; the stale "no such variant here yet" is corrected
- `kernel/arch/x86/lib/arch/include/arch/x86/cache.hpp` — says where the real line size could be read
- `kernel/arch/x86/lib/arch/include/arch/x86/cpuid_observer.hpp` — the field layout cites the Intel and AMD manuals directly
- `kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp` — `SyscallFrame`'s comparison paragraph is dropped; its first sentence already names the stub that builds it
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — destroy()'s assertions, switch_context()'s bit order and null branch, and invalidate()'s batching are stated in this kernel's terms
- `kernel/arch/x86/mm/fault.cpp` — the resume checks, the preamble depth, the atomic-context checks, the ring-0 refusal, the instruction bit, the preemption state and when interrupts come on
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp` — two comparison lines dropped
- `kernel/arch/x86/mm/srat.cpp` — the comparison sentence is dropped
- `kernel/phys/boot_options.cpp` — `<size>@<address>` is described by its fields
- `kernel/phys/image/physboot/BUILD.gn` — `$current_cpu`'s reason stands alone
- `kernel/phys/include/ours/phys/boot_options.hpp` — the crashlog comparison is dropped
- `kernel/phys/include/ours/phys/boot_package_input.hpp` — the page-alignment reason stands alone
- `kernel/phys/include/ours/phys/handoff.hpp` — `Handoff` says what it carries and why, and where userboot and the vDSO travel instead
- `kernel/phys/include/ours/phys/kernel_package.hpp` — the flat model is described without a foreign item type
- `kernel/phys/include/ours/phys/kernel_package_handoff.hpp` — the comparison sentence is dropped
- `kernel/phys/include/ours/phys/module.hpp` — how a module declares its entry, without a comparison
- `kernel/phys/kernel_elf_binary.gni`, `kernel/phys/kernel_elf_image.gni`, `kernel/phys/phys_executable.gni`, `kernel/phys/shim_executable.gni` — the templates' parameters and layout are described by what they do
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp` — the type-mapping paragraph is dropped
- `docs/changes/261004/comment-rule.md` — this entry
- `docs/repairs/261004/comment-rule.md` — `faults.cpp`, which this sweep could not touch

**Symptom**
None; this carries out the user's decision 3 of 2026-10-04 evening, the sweep of rule 2 (CLAUDE.md: code comments say what a thing is and why, in this codebase's own terms) over the group `kernel/arch` and `kernel/phys`.

**Cause**
A comment that explains this kernel by pointing at another kernel is wrong in three ways, and this group had examples of each.

- **It goes stale where nobody looks.** `XApic::wait_icr_idle()` said that one kernel keeps a bounded wait for the NMI and bring-up paths and that "there is no such variant here yet". `try_wait_icr_idle()` sat twenty lines below it and `try_send_ipi()` already used it for INIT and SIPI. The half of the sentence about the other kernel could not go stale, so nobody re-read it; the half about ours did. `idt.S` had the same failure in a smaller form: "code in idt.c assumes [the CLAC] is here" named a file that exists only in the tree the stub was copied from, while the code that really depends on it, `x86_init_idt_early()` in `idt.cpp`, points every gate three bytes past the CLAC on a CPU without SMAP.
- **It hides the invariant behind the comparison.** `HPET_CLKEVT_RATING` was justified as "the HPET must win the comparison it would lose on Linux's numbers". The invariant is about ours alone: once the legacy replacement route takes IRQ0 from the PIT, the device that owns the interrupt must be the one TickDevice is bound to; rated below the PIT, the HPET would leave TickDevice on a device whose interrupt no longer arrives. Likewise `fault.cpp` explained why resolving a fault is not preemptible through another kernel's `PreemptReenable()` and its definition of the eager count; the reader had to learn that kernel to see the reason, which is one sentence about ours: the preamble raised the eager-reschedule count too, and `is_preemption_enabled()` reads it.
- **It reads like a reason while carrying none, or carries a gap without saying so.** "Status::TimeOut is -ETIME" mapped a status onto an errno that nothing in ours uses. "Linux's rating for the local APIC timer" and "the figure Linux warns at" gave a number a pedigree, not a reason; the reason (per-CPU beats global before ratings are compared; noise of a few percent against a failure that is a factor) was already there. In the other direction, `io_apic.cpp` said the EOI of an old IOAPIC "is simply not sent, where Linux falls back to toggling the entry": the only statement that ours lacks that fallback was phrased as a difference. It now says directly that such a chip needs the toggle and that it is not implemented, with the repair item.

Where a design does follow another system, that is worth recording, but in the change record's Reference field and the design documents, where it is dated and attributed. This entry's Reference field keeps every comparison the comments carried, so no learning material is lost.

**Change**
Only comment text changed; no token of code (every changed line in the diff is a comment line, checked with `git diff -U0 | grep -E '^[+-]' | grep -v -E '^[+-]\s*(//|#|\*|/\*)'`, which prints nothing). Two representative rewrites:

```cpp
// xapic.hpp, before
    /// Unbounded, like Linux's __xapic_wait_icr_idle(). Linux keeps a bounded
    /// safe_apic_wait_icr_idle() for the NMI and CPU-bring-up paths, where a
    /// wedged ICR must not take the sender down with it; there is no such
    /// variant here yet.

// after
    /// Unbounded. The NMI and CPU-bring-up paths, where a wedged ICR must not
    /// take the sender down with it, want the bounded try_wait_icr_idle()
    /// below instead.
```

```cpp
// hpet.cpp, before
/// Deliberately above the PIT's 110, where Linux puts the HPET at 50.
///
/// In Linux the two never compete: hpet_time_init() tries the HPET first and
/// only registers the PIT if that fails, ... The device that owns the
/// interrupt has to be the one TickDevice is bound to, so the HPET must win
/// the comparison it would lose on Linux's numbers.

// after
/// Deliberately above the PIT's 110.
///
/// The PIT is enrolled first (arch_time_init(), at Irq + 1) and the HPET after
/// (Irq + 2), and enabling the legacy replacement route below takes IRQ0 away
/// from the PIT -- ... The device that owns the interrupt has to be the one
/// TickDevice is bound to, so the HPET must win the comparison; rated below
/// the PIT, it would leave TickDevice on a device whose interrupt no longer
/// arrives.
```

The rest follow three patterns. A paragraph that was nothing but a comparison ("Zircon counterpart: ...", "Linux does the same in ...") is dropped. A sentence that put a reason in comparative form keeps the reason and loses the comparison: "Refuse that one entry and keep the rest, as Linux's mp_override_legacy_irq() does" becomes "Refuse that one entry and keep the rest". A comparison that was the only statement of a fact about ours is rewritten into that fact: "Zircon also accepts 0 ... This does not" becomes "0 is refused, although a test that wants a thread to fault at once could use it"; "Linux reads the size from CPUID leaf 1 instead" becomes "CPUID leaf 1 reports the real size (EBX bits 15:8, in 8-byte units) should it ever be needed"; "Zircon's physboot goes further and hands the kernel those two as ready ELF images" is dropped, since the sentence before it already says the kernel reads userboot and the vDSO out of the package.

The hits were found with the sweep's grep, `(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)`, over the C++, assembly, linker-script, GN and Python files of the two directories, and with a second grep for foreign names the first cannot see (`counterpart`, `mirrors`, `pfe_handler`, `native_`, `PhysHandoff`, `memalloc`, `sysret_problem`, `.cc`/`.c` paths, `-ETIME`), which found `idt.S`'s `idt.c`. Neither grep sees a link into another project's bug tracker; a review pass grepping for URLs found the one in `kernel/arch/x86/BUILD.gn`.

**Reference**
The comparisons the comments carried, kept here for the reader who wants to compare:

- Rust seam (`kernel/arch/BUILD.gn`): Zircon's `rustc_library("arch-rs")` and its C++ FFI side; Zircon's RISC-V `hypervisor` target is also an empty group.
- RISC-V `ArchCpuState`: Zircon's riscv64 `interrupt_saved_state_t` is likewise a plain bool.
- IOAPIC: Linux `__eoi_ioapic_pin()` falls back to toggling an entry to edge and back on an IOAPIC older than 0x20; Linux `mp_override_legacy_irq()` refuses one bad override and keeps the rest.
- Local APIC timer: Linux rates it 100 (`lapic_clockevent`, `arch/x86/kernel/apic/apic.c`); Linux `local_apic_timer_interrupt()` takes `this_cpu_ptr(&lapic_events)`; Linux `calibrate_APIC_clock()` calls `wait_for_a_tick()`, measures over `HZ/5` (200 ms) and warns at 10%; 0xF and 0x7FFFFFFF are the delta bounds Linux gives `lapic_clockevent` (`min_delta_ticks`, `max_delta_ticks`, set in `calibrate_APIC_clock()`).
- `bootstrap16.S`: the `rva()` trick is Linux's (`arch/x86/boot/compressed/head_64.S`); Linux `secondary_startup_64` pushes a null return address.
- TSS and GDT (`descriptor.cpp`): Linux reads the CPU number back from a per-CPU GDT entry's limit (`LOAD_CPU_AND_NODE_SEG_LIMIT`, used by `GET_PERCPU_BASE`); the two IST limits are as in Zircon, whose IST assignment is `x86_tss_assign_ists()` from `x86_initialize_percpu_tss()`; Linux `entry_64.S` carries a nested-NMI protocol; Linux `native_load_tr_desc()` loads the writable GDT for `ltr` and puts the read-only fixmap copy back; Zircon `gdt_setup()` instead maps the GDT's real pages read-write and only the unused rest of its 64 KiB read-only.
- `paranoid_common` (`exception.S`): Zircon's `.Lnmi` branch of `interrupt_common`, which brackets `x86_nmi_handler()` with `rdmsr`/`wrmsr` of `MSR_IA32_GS_BASE`; Linux `paranoid_entry`/`paranoid_exit` in `arch/x86/entry/entry_64.S`.
- IDT (`idt.cpp`, `idt.S`): Zircon `idt_setup()` skips the CLAC the same way; Zircon `x86_init_percpu()` assigns IST to #DF, NMI and #MC (`NMI_IST_INDEX` and friends) and leaves #DB alone; the stubs' "code in idt.c assumes it is here" is Zircon's sentence, from `kernel/arch/x86/exceptions.S`, where it named Zircon's own file, and so is its "restored by iretd".
- Retpoline (`kernel/arch/x86/BUILD.gn`): Zircon's `config("retpoline")` passes `rustflags = [ "-Zretpoline" ]` under clang; the TODO linked Fuchsia bug `fxbug.dev/507570739`, which Zircon's `kernel/switch/BUILD.gn` also cites.
- Address-space layout: Linux `VMALLOC_START`/`VMALLOC_END`; Zircon `USER_ASPACE_BASE` is also 2 MiB; leaving the top page out is the first measure of Zircon's `docs/concepts/kernel/sysret_problem.md`; Zircon uses `(1 << 46) - USER_ASPACE_BASE` for restricted-mode address spaces.
- User entry (`entry.hpp`): Zircon `arch_is_valid_user_pc()` (`kernel/arch/x86/include/arch/vm.h`) also accepts 0; Zircon `arch_enter_uspace()` asserts interrupts are off and checks the entry point in every build; Linux and Zircon both start user threads at IOPL 0.
- User copies (`thread.hpp`, `user_copy.hpp`, `user_copy.S`, `user_copy.cpp`, `fault.cpp`): Zircon `X86_PFR_RUN_FAULT_HANDLER_BIT` (`arch/x86/include/arch/arch_thread.h`), bit 63 of `page_fault_resume`, read by `x86_pfe_handler()`, which keeps the resume address alone (the admitted range is ours's addition); Zircon `arch_copy_from_user()`/`arch_copy_to_user()` (`kernel/arch/x86/user-copy/user-copy.cc`) pick a variant by code patching, from four alternatives (quadword or `rep movsb`, with or without SMAP, `code-patches.cc`), `user-copy.S` assembled with `-DSMAP` and `-DMOVSB`; Linux `arch/x86/lib/copy_user_64.S`; Zircon `UserCopyCaptureFaultsResult` (`kernel/include/arch/user_copy.h`) with `pf_va` and `pf_flags`, and `arch_copy_*_user_capture_faults()`; Zircon `_arch_copy_to_from_user()` puts an lfence between `can_access()` and the copy and asserts AC clear after `_x86_copy_to_or_from_user()`; a fault in a copy returns `ZX_ERR_INVALID_ARGS` there too.
- Clock-event naming (`time.hpp`, `time.cpp`): Linux `global_clock_event`.
- Selectors (`descriptor.hpp`): Zircon `USER_CODE_64_SELECTOR` is `(0x30 | 3)` and `USER_DATA_SELECTOR` `(0x28 | 3)`, over the same GDT layout.
- Per-CPU (`percpu.hpp`): Linux `__percpu_rel` in `arch/x86/include/asm/percpu.h`; Zircon `x86_reset_tss_io_bitmap()` (`ioport.cc`), called before `ltr` in `x86_initialize_percpu_tss()` (`descriptor.cc`).
- SYSCALL (`init.cpp`, `syscall.S`, `syscall.hpp`): Linux `SYSCALL_MASK` in `syscall_init()` (`arch/x86/kernel/cpu/common.c`); Zircon `x86_init_percpu()` (`kernel/arch/x86/mp.cc`), `X86_FLAGS_STATUS_MASK` 0xfff plus AC; Linux `entry_SYSCALL_64` saves the user stack through `TSS.sp2` into `pt_regs->sp` and ends its fast path with `popq %rsp`; Zircon's `syscall.S` calls `x86_syscall_process_pending_signals()` only when `do_syscall()` reports a pending signal; Linux `do_syscall_64()` checks the return address and falls back to IRET, Zircon keeps bad values out instead (`sysret_problem.md`); Zircon `x86_syscall_general_regs_t` (`zircon/kernel/arch/x86/include/arch/x86/general_regs.h`) and the argument order of its `syscall.S`.
- IPIs and SMP (`mp.cpp`, `xapic.hpp`): Zircon `mp_mbx_reschedule_irq()` leaves the EOI to the `X86_INT_IPI_RESCHEDULE` case in `x86_exception_handler()`; Linux `do_boot_cpu()` waits a bounded time and reports "CPU%d: didn't respond"; Linux `setup_local_APIC()` writes `APIC_SPIV_APIC_ENABLED | SPURIOUS_APIC_VECTOR` on every CPU; Linux `__xapic_wait_icr_idle()` beside `safe_apic_wait_icr_idle()`; Linux `__default_send_IPI_dest_field()` waits for idle, then writes ICR2, then ICR; Linux `default_send_IPI_mask_sequence_phys()` takes `local_irq_save()` once for the whole set.
- Idle (`idle_thread.cpp`): Zircon's x86 `arch_idle()` is the same `sti; hlt`.
- HPET (`hpet.cpp`): Linux rates the HPET clockevent 50 and never makes it compete with the PIT (`hpet_time_init()` registers the PIT only if the HPET fails); Linux rates the HPET clocksource 250, between the ACPI PM timer's 200 and the TSC's 300; Linux `hpet_next_event()` and its "within `HPET_MIN_CYCLES`" verdict, returning `-ETIME`; Linux `hpet_set_periodic()` writes the comparator twice; Linux checks the main counter runs before registering the clocksource; Linux bounds the period to 1e5..1e8 fs.
- Cache (`cache.hpp`): Linux reads the line size from CPUID leaf 1 (`x86_clflush_size`).
- CPUID (`cpuid_observer.hpp`): Zircon `CpuidAddressSizeInfo` (`lib/arch/include/lib/arch/x86/cpuid.h`).
- Page tables (`x86_pagings.hpp`): Zircon gives Global to the kernel address space alone (`X86PageTableMmu::terminal_flags()`, `use_global_mappings_`); Zircon `intermediate_flags()` is the same RW|U.
- Address-space switch and teardown (`arch_vm_aspace.cpp`, `arch_vm_aspace.hpp`): Zircon `X86ArchVmAspace::Destroy()` asserts `active_cpus_ == 0`; Zircon `X86ArchVmAspace::ContextSwitch()` keeps `active_cpus_` but clears the outgoing bit first, and writes `root_page_table_phys` when the aspace is null, since null is how Zircon spells a kernel thread; Zircon `X86PageTableMmu::TlbInvalidate()` `mp_sync_exec()`s to `active_cpus_`, and to every CPU for a global entry; Linux `flush_tlb_mm_range()` over `mm_cpumask()` and `flush_tlb_kernel_range()` on every CPU, which Linux batches lazily for vmalloc.
- Page faults (`fault.cpp`): Zircon `int_handler_start()` raises the same two counts as `irq::handle_irq_preamble()`; Linux `do_user_addr_fault()` reads IF from the saved frame and enables interrupts before taking `mmap_lock`; Linux `faulthandler_disabled()` and `in_atomic()` fold both atomic checks into one preempt counter; Zircon `x86_pfe_handler()` dies on a ring-0 fault without a resume address ("page fault in kernel mode"), printing "potential SMAP failure" first when SMAP explains it, maps only `PFEX_I` to `VMM_PF_FLAG_INSTRUCTION`, tests `X86_PFR_RUN_FAULT_HANDLER_BIT` before enabling anything, `DEBUG_ASSERT`s `arch_num_spinlocks_held()`, calls `PreemptReenable()` while leaving the eager count, and enables interrupts at the same point.
- NUMA (`srat.cpp`): Linux `numa_set_node()` into `early_per_cpu(x86_cpu_to_node_map)`.
- Boot options (`boot_options.cpp`, `boot_options.hpp`): Linux `memmap=` and `crashkernel=` take `<size>@<address>`; Linux ramoops takes `ramoops.mem_size` and `ramoops.mem_address`.
- Phys handoff (`boot_package_input.hpp`, `handoff.hpp`, `kernel_package.hpp`, `kernel_package_handoff.hpp`, `module.hpp`, `bootmem/types.hpp`): Zircon physboot keeps the data ZBI page-aligned and rounded out (`MakePhysVmo()` in `zircon/kernel/phys/handoff-prep.cc`); Zircon `PhysHandoff`, `PhysMapping` grouped into `PhysVmar` (`kernel/phys/include/phys/handoff.h`), and `PhysHandoff::userboot` and `::vdso` as `PhysElfImage`; the flat kernel package matches Zircon's `STORAGE_KERNEL` item; a phys module declares its entry the way a Linux module does with `module_init()`; Zircon `memalloc::Type` (`kernel/phys/lib/memalloc`), whose `kFreeRam`, `kKernelPageTables`, `kDataZbi` and `kPhysScratch` are `Free`, `PageTable`, `BootPackage` and `Transient` here.
- Phys GN templates: `kernel_elf_binary` mirrors Zircon's public target topology; Zircon obtains the unstripped ELF through link-output metadata, defaults `has_patches` to true, and puts a patched kernel in an `$install_name/` subtree; `emulator_support_aib` is Zircon's emulator-support assembly-input metadata seam; Zircon's template has a Fuchsia product adapter that chooses a global QEMU-default shim and archive policy; Fuchsia spells `$current_cpu` the same way.

**Verification**
- `scripts/format_cpp.py --check` on every changed C++ file and `scripts/format_gn.py --check` on every changed GN file: clean.
- Build and boot, `boot_check.py` on a fresh `out/bringup.x86`: build ok in 48.7 s; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines).
- Host smoke tests that include the touched headers, all exit 0: `kernel/lib/arch` `cpu-mask-smoke`, `x86-feature-smoke`, `x86-paging-smoke`, `x86-system-smoke`; `kernel/phys/lib/bootshim` `legacy-memory-smoke`; `kernel/phys/lib/multiboot` `memory-map-smoke`, `view-smoke`.
- The two greps above, rerun after the change: the hits left are listed under "Deliberately not done".
- clang-tidy and the kernel shell tests: not run; the change touches comment text only, which neither checks.
- Review: comment-only confirmed a second way, by comparing each changed C++ and assembly file's clang raw-token stream, comments dropped, against `mvp/integration` (identical for all 40 files) and each GN file with its comment lines dropped (identical for all 7). After the review's fixes (`kernel/arch/x86/BUILD.gn`, `idt.S`, `syscall.hpp`), `boot_check.py`: dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines).

**Deliberately not done**
- `kernel/arch/x86/kernel/faults.cpp` keeps its twelve hits (Zircon's exception mapping, its `#CP` handling, its NMI counters, `try_dispatch_user_exception()`, `arch_dump_exception_context()`, `x86_syscall_process_pending_signals()`, `x86_nmi_handler()`, `x86_exception_handler()`'s order; Linux's NMI printk and `noinstr`): another session is changing that file, and its working copy still carries them. Recorded as `docs/repairs/261004/comment-rule.md` item 1.
- `kernel/phys/build_id_note.cpp`'s "The universal GNU/Linux note convention" stays: it states a fact about the ELF note ABI the toolchain emits (4-byte alignment whatever the ELF class), not another kernel offered as a reference, which rule 2 leaves alone.
- `kernel/arch/arm/lib/arch/include/arch/arm/interrupt.hpp`'s "mirrors x86's ArchCpuState" compares two parts of this tree, not another system, so it stays.
- The license notices ("BSD-style license", "MIT-style license") and the GNU copyright header of `kernel/phys/lib/multiboot/include/multiboot/multiboot2.hpp` are not touched. `kernel/phys/README.md` and `kernel/phys/lib/bootmem/README.md` are documents, which rule 2 does not touch; neither names another system anyway.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the HPET's clock-event rating be above the PIT's once the legacy replacement route is enabled, and what would TickDevice be left with if it were not?
- Why can a page fault be resolved with blocking allowed but without preemption, given what the exception preamble raises and what `is_preemption_enabled()` reads?
- Why does a stub's CLAC have to be the first instruction, and how does `x86_init_idt_early()` avoid a `#UD` on a CPU without SMAP without writing to kernel text?

## Title: the kernel core's comments say what a thing is and why, without naming another system (2026-10-04)

**Files**
- `kernel/kernel/irq/irq_object.cpp` — attach() and detach() keep why a Permanent line refuses changes, without a foreign set_handler(); notify_all() and notify() say what they do and that a line firing for nobody is counted, never shut off
- `kernel/kernel/irq/mod.cpp` — handle_irq_preamble() and handle_irq_postamble() say what they are (the entry and exit bookkeeping of an interrupt) instead of naming foreign functions; the note on the missing hard-IRQ count keeps its pointer to PreemptionState's Tag
- `kernel/kernel/sync/event.cpp` — wait_etc() and signal() keep the latching rule and the IrqSave reasons, without the foreign Event and Guard they were checked against
- `kernel/kernel/sync/mutex.cpp` — six comparisons dropped; the spin-to-block rule and the CAS-loop-instead-of-fetch_or reason are stated directly
- `kernel/kernel/sync/semaphore.cpp` — why the semaphore needs no repair step after an interrupted wait, stated as the design it avoids (a count that goes negative per waiter)
- `kernel/kernel/task/cpu_stopper.cpp`, `dpc.cpp`, `idle_thread.cpp`, `init.cpp`, `ipi_service.cpp`, `owned_wait_queue.cpp`, `preemption_state.cpp`, `wait_queue.cpp` — comparisons dropped or turned into the alternative and why it is not taken (a kernel thread's null aspace, copying a Dpc before invoking it, an eager-resched tag lockdep lacks); WaitQueue::handle_timeout() says what it does; park_and_assign_owner() says "no one lock over every thread" instead of naming a foreign global `thread_lock`, and SyncCall says what makes it allocation-free (living on the caller's stack) instead of a bare "It"
- `kernel/kernel/task/sched/scheduler.cpp` — the context-switch, idle-mask, tick-lock-order and CPU-selection comments keep their reasons; the scheduler-class uniqueness check is explained as what a per-discipline linker slot would make structural; the two comments on the PI-lock hand-over name complete_context_switch(), the function that drops the previous thread's lock, instead of `finish_switch_context`, a name nothing in the tree has
- `kernel/kernel/task/thread.cpp` — twenty-odd comparisons in create_first(), suspend_or_kill(), join(), detach(), preempt(), sleep_etc(), kill() and exit() dropped; join()'s contract says what the Joined claim stands in for; detach() says why waking the joiner with BadState is refused
- `kernel/kernel/time/clock_event_device.cpp`, `clock_source.cpp`, `tick_device.cpp`, `tick_sched.cpp`, `timer.cpp`, `timer_queue.cpp` — functions introduced by a foreign function's name now say what they do (check_percpu(), check_preferred(), rebind_device(), setup_periodic(), setup_oneshot(), handle_periodic_event(), on_tick(), Timer::forward(), Timer::cancel(), run_expired_timers()); the reasons that were stated as "Linux does X because Y" keep Y
- `kernel/platform/include/ours/platform/power.hpp`, `kernel/platform/pc/power.cpp` — the reset methods say what they are and in which order they are tried, without the foreign functions that write the same bytes
- `kernel/platform/pc/interrupt.cpp` — the vector-per-GSI choice and the ISA-2 skip keep their reasons; the "first ISA line keeps the GSI" rule says what the other order would do
- `kernel/top/main.cpp` — labour_routine()'s note says the INIT_CODE split in this tree's terms; start_nonboot_cpu()'s comment says how it ends
- `kernel/top/panic.cpp` — claim_report(), stop_this_cpu(), quiesce_machine(), the banner, the ring replay and vpanic()'s print order keep their reasons, without four foreign panic paths
- `kernel/commands/crash.cpp` — the model sentence dropped
- `kernel/commands/user_copy_test.cpp` — a caller retrying a faulted copy goes through soft_fault(); the comment named a foreign `SoftFaultInRange()` instead
- `kernel/kernel_shell_script.gni`, `kernel/user_bootfs.gni`, `kernel/products/bin/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` — comparisons dropped; vmours.with-tests says what it is (vmours with `//kernel/tests:kernel.tests` linked in)
- `kernel/products/image/BUILD.gn` — the header and the common_package_items comment, which were Chinese and named the foreign image directory three times, rewritten in English with every reason kept; the subdirectories it lists are the ones that exist (dev/, dev.dbg_1/, fake/), not the stale kernel.dev/ and kernel.dev.dbg_1/ the Chinese text carried
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
None; this carries out the user's decision 3 of 2026-10-04 evening, the sweep of rule 2 (CLAUDE.md: code comments say what a thing is and why, in this codebase's own terms) over the kernel core: `kernel/kernel`, `kernel/top`, `kernel/platform`, `kernel/commands`, `kernel/products`, `kernel/tests` and the files directly in `kernel/`.

**Cause**
The kernel core is where the habit was densest: about 130 comment lines across 32 files explained this code by the function in another kernel it was checked against. That habit is a mistake for four reasons, and the core showed all of them.

- **It hands the reader a second codebase to read.** "Linux's tick_check_preferred()" as the whole introduction of TickDevice::check_preferred() tells someone who knows only ours nothing about what the function decides; the two rules it applies were in the next paragraph, and the first line now says what the function answers. The same held for "Zircon's int_handler_start()", "tick_setup_periodic()", "Linux's hrtimer_forward()" and "Zircon's Timer::Cancel(), step for step": a function's first line is the one a reader sees in a hover or a grep, and it named someone else's function instead of saying what this one does.
- **A comparison can stand in for a reason without being one.** "Zircon's SuspendOrKillInternal() runs its whole transaction under PreemptDisableAndIrqSaveOption" read as a justification, but the justification was the sentence before it: a thread killing itself takes a timer interrupt while holding its own PI lock, and the interrupt's exit path takes that lock again on the same CPU. Wherever the comparison was attached to a real reason, the reason stays and the comparison goes; where a comparison was the only content of a sentence ("Zircon asserts the same", "Zircon sizes its equivalent array the same way"), the sentence goes.
- **It goes stale without anyone noticing.** Several comments cited the other tree by path (`zircon/kernel/platform/pc/power.cc`, `kernel/debugcommands/debugcommands.cc`, `//zircon/kernel/image:common-package-items`). No build checks either side; when one side moves, the comment points at something that no longer says what it claims. A statement about ours's own invariant is checked by every reader who looks at the code next to it.
- **It can hide a statement that was only true of the other system.** timer_queue.cpp said run_expired_timers() was "Zircon's TimerQueue::Tick() with Linux's per-base lock", but the lock in this TimerQueue is one per queue (`m_lock`), not one per clock base. Restating the comment in ours's terms forced the check, and it now says "under the queue's lock". Likewise check_preferred()'s "Linux rates the local APIC below the PIT" became a claim about ours that was then checked against the tree: the local APIC enrolls at rating 100, the PIT at 110 and the HPET at 120.

Where a design does follow another system, that is worth knowing, but it belongs where it is dated and attributed: in a change record's Reference field and in the design documents. This entry's Reference field carries everything the comments used to cite, so nothing is lost to a reader who wants to compare.

**Change**
Only comment text changed; no token of code. Three representative rewrites:

```cpp
// before (kernel/kernel/time/tick_device.cpp)
/// Linux's tick_check_percpu(), minus one clause. Linux also refuses a global
/// device whose interrupt cannot be re-targeted to this CPU
/// (irq_can_set_affinity()); the IRQ layer here has no such query yet, and

// after
/// Whether |newdev| may serve as this CPU's tick device at all. One clause is
/// missing: a global device whose interrupt cannot be re-targeted to this CPU
/// should be refused too, but the IRQ layer here has no such query yet, and
```

```cpp
// before (kernel/top/main.cpp)
    /// Same split as Linux: the `__init` work is reached through calls
    /// (init_arch/init_platform, as `kernel_init_freeable()`), while the
    /// function that outlives the reclaim is not `__init` itself
    /// (`kernel_init()`, marked `__ref`).

// after
    /// Hence the split: the INIT_CODE work is reached through calls
    /// (init_arch(), init_platform()), while the function that outlives the
    /// reclaim is not INIT_CODE itself.
```

```cpp
// before (kernel/kernel/task/sched/scheduler.cpp)
    // Linux does not need the check because it does not have the collapse:
    // DEFINE_SCHED_CLASS gives every class its own slot in the linker script,
    // so uniqueness is structural. Flattening that into an array indexed by an
    // enum is what moves the obligation here.

// after
    // Giving every discipline its own slot in the linker script would make
    // uniqueness structural and the check unnecessary. Flattening the
    // registrations into an array indexed by an enum is what moves the
    // obligation here.
```

The other rewrites follow the same three patterns: a function's first line says what it does instead of whose function it is; "X does the same for the same reason" is dropped when the reason is already stated next to it; and "ours departs from X, which does Y" becomes "Y is not done, because ...". A few comments were made more precise while being restated: handle_irq_preamble() now says what its bookkeeping is (record the frame, raise the counts that keep the CPU from switching threads until the exit), and Timer::cancel() says what its return value means (true when the handler had not started, and now never will).

`kernel/products/image/BUILD.gn` is the one place where more than the comparison changed. Its two comments were in Chinese, badly wrapped, and named the foreign image directory three times; since rule 2 required rewriting them anyway, and the repository writes new text in English, they were rewritten in English with every reason kept (why userboot hangs off the shared group, why it names the user toolchain, why //examples/shell no longer lives there). The Chinese comments in `kernel/products/bin/BUILD.gn`, which name no other system, were left as they are.

The hits were found with the sweep's grep, `(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)`, over the C, C++, assembly, linker-script, GN and Python files of the seven places, and with a second grep for foreign identifiers the first cannot see (`hrtimer`, `clockevents`, `tick_setup`, `__init`, `irq_desc`, `mp_sync`, `smp_`, `kmsg`, `ONESHOT_STOPPED`, "counterpart", "-style"). The second grep's only hits were ours's own identifiers (`AutoPreemptDisabler`, `IrqSaveAndPreemptDisable`) and "ChainLock-style" in `owned_wait_queue.cpp`, which names ours's own `concurrent::ChainLock` and `kconcurrent::ChainLockTransaction`, so it stays. The review added a third check that neither grep makes: every identifier in a comment that no line of code in the tree defines or uses. It found two foreign names the word lists missed, `SoftFaultInRange()` in `user_copy_test.cpp` and a global `thread_lock` in `owned_wait_queue.cpp`, and one stale ours name, `finish_switch_context`, in `scheduler.cpp`; all three are restated.

**Reference**
The comparisons the comments carried, kept here for the reader who wants to compare:

- IRQ: Zircon `InterruptTableEntry::set_handler()` (refuses a handler on, and clearing of, a permanent entry); Linux `__handle_irq_event_percpu()` (every action on a shared line runs) and `note_interrupt()` with `irq_desc::irqs_unhandled` (which also disables a line that keeps firing for nobody); Zircon `int_handler_start()` and `int_handler_finish()`; Linux `irq_enter_rcu()`, which bumps a hard-IRQ count in `preempt_count`.
- Sync: Zircon `Event::WaitWorker()` and `Event::SignalEtc()` (`AUTOUNSIGNAL`), whose parks run under `PreemptDisableAndIrqSave` and whose `Guard::Release()` restores the saved interrupt state with the lock; Zircon `Mutex::AcquireCommon()`, `AcquireContendedMutex()` (one `fetch_or`), `Mutex::TryRelease()`, `ReleaseContendedMutex()` (`STATE_FLAG_CONTESTED`) and its spin deadline through `timer_get_ticks_to_time_ratio()` ("All spinners convert to blocking when the first one reaches the max spin duration"); Zircon `Semaphore::Post()`, whose count goes negative per waiter and is repaired by the next post.
- Task: Linux `stop_one_cpu_nowait()`, `cpu_stopper_thread()`, `cpu_stop_get_work()`; Zircon `DpcRunner::Queue::DoWork()` (copies the Dpc before invoking it); Zircon `IdlePowerThread` (starts at `kActive`); Zircon `X86ArchVmAspace::ContextSwitch()` (a kernel thread's null `aspace_`); Zircon `mp_sync_context`, `mp_ipi_task`, `mp_sync_exec()`, `mp_mbx_generic_irq()`; Linux `smp_send_stop()`, `smp_call_function_many()`; Zircon `OwnedWaitQueue::LockForWakeOperationOrBackoff()`, `SetThreadBaseProfileAndPropagate()` (`EagerReschedDisableAndIrqSaveOption`); Zircon `PreemptionState::FlushPendingContinued()`; Zircon's thread trampoline (`LockHandoff`, then `arch_enable_ints()`), `PreemptLocked()`, `RescheduleMask()`, `LockHandoffInternal()`, `Scheduler::Unblock()`, `UnblockCommon()`, `mp.idle_cpus.fetch_or()`; Linux `DEFINE_SCHED_CLASS`, `switch_to()`'s `last`, `find_idlest_cpu()`, `select_task_rq()`; Zircon `thread_construct_first()`, `SuspendOrKillInternal()`, `Thread::Join()`, `TaskState::Join()`, `EraseFromListsLocked()`, `Thread::Detach()`, `Preempt()`, `SleepEtc()`, `Thread::HandleSleep()`, `Thread::Current::Kill()`, `Thread::Current::Exit()` ("somehow fell through thread_exit()"), `WaitQueue::TimeoutHandler()`, the global `thread_lock` that chain locking replaced; Linux `try_to_wake_up()`, `signal_wake_up_state()`, `kick_process()`.
- Time: Linux `clockevents_program_event()`, `clockevents_program_min_delta()`, `CLOCK_EVT_FEAT_KTIME`, `set_state_shutdown`, `clocksource_select()`, `CLOCK_SOURCE_VALID_FOR_HRES`, `cpu_none_mask`, `hrtimer_reprogram()`, `ONESHOT_STOPPED`, `tick_check_percpu()` (with `irq_can_set_affinity()`), `tick_check_preferred()`, `tick_install_device()`, `tick_setup_device()`, `clockevents_exchange_device()`, `tick_do_timer_cpu`, `tick_setup_periodic()`, `tick_setup_oneshot()`, `tick_handle_periodic()`, `tick_periodic()`, `update_process_times()`, `account_process_tick()`, `timekeeping_valid_for_hres()`, `hrtimer_forward()`, `__hrtimer_get_next_event()` with `hrtimer_clock_base::offset`; Zircon `Timer::Cancel()` and `TimerQueue::Tick()`.
- Platform: Zircon `reboot()` in `zircon/kernel/platform/pc/power.cc` through `platform_halt(HALT_ACTION_REBOOT)`, `cf9_reboot_system()`, `pc_keyboard_reboot()`; Linux `BOOT_TRIPLE` in `native_machine_emergency_restart()`; Zircon `InterruptManager::register_interrupt_handler()` (a vector from a pool) and `platform_init_apic()` (skips ISA 2); Linux `mp_config_acpi_legacy_irqs()` (maps overrides first).
- Top: Linux `__init`, `kernel_init_freeable()`, `kernel_init()` (`__ref`), `start_secondary()`; Zircon `platform_panic_start()` (exchange on `panic_started`) and `DLog::BluescreenInit()` ("ZIRCON KERNEL PANIC"); Linux `panic()` (compare-exchange on `panic_cpu`, `smp_send_stop()` before printing, "Kernel panic - not syncing" before `dump_stack()` and `kmsg_dump()`), `panic_smp_self_stop()`.
- Commands: Zircon `VmAspace::SoftFaultInRange()`, which a caller retrying a faulted user copy calls.
- Build: Zircon's `crash` debug command (`kernel/debugcommands/debugcommands.cc`); Zircon's `kernel.shell.script` encoding, `userboot.next`, the ZBI's `STORAGE_KERNEL` and `STORAGE_BOOTFS`; Fuchsia `//zircon/kernel/image`, `kernel_package("zircon")` and `//zircon/kernel/image:common-package-items` (userboot, the vDSO and `version-string.txt`); the `//kernel/bin` directory seam and binary topology.

**Verification**
- No code changed: each of the 32 touched files, with its comments stripped and its whitespace collapsed, is the same before and after (a scratch script that strips `//` and `/* */` comments outside string literals from C++ and `#` comments from GN, and compares HEAD with the working tree).
- `scripts/format_cpp.py --check` on the 27 C++ files: clean. `scripts/format_gn.py --check` on the five GN files: four clean; `kernel/products/bin/BUILD.gn` is reported "unsafe" (gn's layout would change more than layout there), which the formatter and the pre-commit hook leave alone; the HEAD version of that file is reported the same way, so it predates this change.
- Build and boot, `boot_check.py`: on the worktree's `out/bringup.x86`, build ok; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines). Again into a fresh `out/kc-fresh`: build ok in 48.1 s, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 2/2 required), no error lines.
- clang-tidy with `-p out/kc-fresh` on the 26 touched `.cpp` files: no diagnostic on a changed line (the pre-existing ones elsewhere in those files and their headers are untouched).
- The sweep's grep, rerun over the seven places: the only hits left are two lines in `kernel/kernel/sampler/sampler.cpp`, kept for the reason below.
- Review: the comment-identifier check above, rerun after the fixup, finds no foreign name outside `sampler.cpp` (`ThreadSampler`, `StopLocked()`). What else it lists is generic (`fetch_or`, `SeqCst`), ours's own vocabulary (`STORAGE_KERNEL`, `PhysBoot`, script names), or code that used to exist and is described as such (`try_wake_joiners`, `total_expected_runtime`). The code-equality check and boot_check.py were rerun on the fixup.

**Deliberately not done**
- `kernel/kernel/sampler/sampler.cpp` lines 19 and 127 still name Zircon: the file belongs to another session that is changing it now, and this run must not touch it. Its comments are the one rule-2 debt left in the kernel core, to be swept once that session lands.
- `kernel/commands/prof.cpp` and `kernel/kernel/task/sched/fair.*` were not opened for editing: the first belongs to the same session, the second to the user (no edits of any kind). Neither had a hit.
- The "BSD-style license" lines in `kernel/*.gni`, `kernel/debug_frame.S` and the products' BUILD.gn files are ours's own license notice, which rule 2 does not touch.
- The Chinese comments in `kernel/products/bin/BUILD.gn` name no other system and were left in Chinese.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must handle_periodic_event() advance only jiffies for a missed period, and not run on_tick() once per missed period as well?
- Why does a thread that suspends or kills itself need its PI lock taken with interrupts off as well as preemption, and what deadlocks if only preemption is off?
- Where does a comparison with another kernel belong in this repository, and what went wrong in timer_queue.cpp's comment that restating it in ours's terms caught?

## Title: the new system libraries' comments say what a thing is and why, without naming another system (2026-10-04)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — the namespace comment states what the Loop leaves out and why its waits live in a caller-sized table; run()'s comparison paragraph becomes the reason a key carries a generation, and the reason a callback never arms its own wait
- `system/lib/component/include/component/decl.hpp` — Decl's comment says the root does not re-run the compiler's checks at resolve, and why, instead of naming another system's validator
- `system/lib/wire/src/wire.cpp` — names_a_status()'s comment says why a peer's u32 status is checked against the enum, without a foreign status type
- `system/lib/wire/test/wire_test.cpp` — the header-offset assertions are introduced by what they pin down, not by a foreign struct's name
- `system/lib/fsio/src/path.cpp` — clean_path()'s body points at its own five rules in `<fsio/path.hpp>` instead of a foreign function it follows; the end-of-name comment drops a foreign NUL comparison
- `system/lib/fsio/include/fsio/path.hpp` — clean_path()'s comment states its five rules without naming the paper and the library they come from
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
None; this carries out the user's decision 3 of 2026-10-04 evening, the sweep of rule 2 (CLAUDE.md, "comments say what a thing is and why") over the group of libraries the component framework added: `system/lib/wire`, `ours-io`, `ours-process`, `ours-kernel`, `vfs`, `fsio`, `component`, `loop` and `svc`.

**Cause**
A comment that explains this code by pointing at another system's code is a mistake for three reasons, and each of the comments here showed at least one of them.

- **It asks the reader to know the other system first.** "Fuchsia's key is the wait's address, so a stale packet would be a use-after-free, which async_loop_cancel_wait prevents with zx_port_cancel_key" is the most important design reason in the Loop, but it was stated as a difference from code the reader has not seen. Someone who knows only ours had to reconstruct the actual invariant themselves: a key must not be something a stale packet can dereference, because ours has no port_cancel to withdraw a packet already queued. Stated directly, the reason is one sentence and needs nothing outside this tree.
- **It goes stale without anyone noticing.** run()'s comment cited `sdk/lib/async-loop/loop.c` by line range and a checkout commit (`51f05104c13`), and Decl's cited `resolving.rs:133-143`. Neither side of that comparison is checked by any build: the other tree moves on, ours changes its own design, and the comment keeps pointing at lines that no longer say what it claims. A comment about ours's own invariant is checked every time someone reads the code next to it.
- **It hides whether the information is a reason or trivia.** "as zx_status_t is a plain int32 in FIDL" and "where fdio finds the NUL" read as reasons but carried none: the first was decoration on "a peer's status is a plain u32", the second described a string representation ours does not use. Removing the comparison showed which sentences carried information about ours and which did not.

Where a design does follow another system, that is worth knowing, but it belongs where it is dated and attributed: a change record's Reference field and the design documents. This entry's Reference field carries everything the comments used to cite, so no learning material is lost.

**Change**
Only comment text changed; no token of code. The largest rewrite, run()'s closing paragraph in `loop.hpp`:

```cpp
// before
    /// Fuchsia's counterpart is async_loop_run() (sdk/lib/async-loop/loop.c:
    /// 327-337) over async_loop_run_once() (:347-407), ...
    /// Two differences: Fuchsia's key is
    /// the wait's address (:23-31), so a stale packet would be a
    /// use-after-free, which async_loop_cancel_wait prevents with
    /// zx_port_cancel_key (:602-634), and ours has no port_cancel yet; and a
    /// Fuchsia handler arms its next wait itself (async_begin_wait, reaching
    /// async_loop_begin_wait at :579-600), where here the callback's answer
    /// tells run() to. Fuchsia checked at 51f05104c13.

// after
    /// Why a key carries a generation: ours has no port_cancel yet, so a
    /// packet already queued cannot be withdrawn. A key that named only where
    /// the wait lives -- a slot's index or address -- would hand a stale
    /// packet, one queued for a slot freed and taken again since, to the
    /// slot's new callback; a key that was the address of a wait object the
    /// caller frees would make the same packet a use-after-free. A stale key
    /// only fails step 1's check, and the packet is dropped. And a callback
    /// never arms its next wait itself: its answer tells run() to (step 3),
    /// so add() arms a slot's first wait and run() every later one.
```

The first rewrite of this paragraph said a key "pointed into memory" would make a stale packet a use-after-free, and that every wait of a slot is armed in run(). Both were wrong for ours: a slot lives in a table that outlives the Loop, so a key naming the slot without a generation is a misdelivery to the slot's new callback, not a use-after-free, and add() arms a slot's first wait. The paragraph above says both precisely, and keeps the use-after-free for the design it belongs to, a wait object the caller frees.

The others, in short:

- `loop.hpp`, namespace comment: "The counterpart of Fuchsia's async-loop (sdk/lib/async-loop/loop.c), cut down to waits: no tasks, ..." becomes "It does waits and nothing else: no tasks, no timers, no threads. Each wait lives in a slot of a table the caller sizes, not in a wait object the caller allocates, because a user program has no heap."
- `decl.hpp`: "Fuchsia re-runs those checks at every resolve instead (cm_fidl_validator::validate, from src/sys/lib/routing/src/resolving.rs:133-143); the design records the departure." becomes "..., so the root does not run those checks again when it resolves a component; the design records that choice."
- `wire.cpp`: "a plain u32 on the wire, as zx_status_t is a plain int32 in FIDL" becomes "a plain u32 on the wire, so it can hold any value".
- `wire_test.cpp`: "The header is fidl_message_header_t's shape, with the direction in byte 4." becomes "Every peer reads the header's fields at these offsets: the txid, the direction in byte 4, two reserved bytes, the magic and the ordinal."
- `path.cpp`: "fdio's CleanPath() (sdk/lib/fdio/cleanpath.cc), step for step, on a string_view instead of a NUL-terminated string. Its step numbers are kept in the comments." becomes "The five rules of clean_path()'s comment (<fsio/path.hpp>), applied in one pass over |path|, a string_view rather than a NUL-terminated string. The numbered comments below are those rules' numbers."; "where fdio finds the NUL" is dropped.
- `path.hpp`: "Cleans |path| into |out| by the rules of Plan 9's "Lexical File Names in Plan 9 or Getting Dot-Dot Right", as Go's filepath.Clean applies them:" becomes "Cleans |path| into |out| by five rules:". The rules follow in full, so the comment loses nothing a reader needs; only the attribution moves to Reference. Plan 9 and Go are not on rule 2's list of names, but rule 2's last sentence covers any design that follows another system.

The hits were found with the sweep's grep, `(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)`, over the C, C++, GN and Python files of the nine directories, and with a second grep for foreign identifiers the first cannot see (`zx_`, `fidl`, `fdio`, `async_`, `sdk/`, `src/sys`, "counterpart"), which found the `wire.cpp`, `wire_test.cpp` and `path.cpp` comments.

**Reference**
The comparisons the comments carried, kept here for the reader who wants to compare (Fuchsia checked at `51f05104c13`):

- Loop: Fuchsia `async_loop_run()` over `async_loop_run_once()` (`sdk/lib/async-loop/loop.c:327-337`, `:347-407`), which takes one packet with `zx_port_wait`, turns a signal packet's key back into the `async_wait_t` it was armed with, and calls its handler through `async_loop_dispatch_wait()`. Fuchsia's key is the wait's address (`:23-31`), and `async_loop_cancel_wait()` withdraws a queued packet with `zx_port_cancel_key()` (`:602-634`); a Fuchsia handler re-arms itself with `async_begin_wait()`, reaching `async_loop_begin_wait()` (`:579-600`).
- Decl: Fuchsia validates a manifest at every resolve, `cm_fidl_validator::validate`, called from `src/sys/lib/routing/src/resolving.rs:133-143`.
- wire: the header layout is Fuchsia's `fidl_message_header_t`, with ours's direction byte in byte 4; a Fuchsia status is `zx_status_t`, a plain int32.
- fsio: `clean_path()` follows Fuchsia fdio's `CleanPath()` (`sdk/lib/fdio/cleanpath.cc`) step for step, which walks a NUL-terminated string. Its five rules are those of Rob Pike's "Lexical File Names in Plan 9 or Getting Dot-Dot Right", as Go's `path/filepath.Clean()` applies them.

**Verification**
- `scripts/format_cpp.py --check` on the six C++ files: clean.
- No code changed: each touched C++ file, with its comments stripped and its whitespace collapsed, is byte-for-byte the same before and after.
- Build and boot, `boot_check.py` on a fresh `out/bringup.x86`: build ok in 35.7 s; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines). Again after the review's corrections to `loop.hpp` and `path.hpp`: build ok; dbg PASS (29 passed, 2 skipped); rel PASS (26 passed, 7 skipped, 2/2 required); no error lines.
- Host tests of the touched libraries, all passed: `loop` 18, `wire` 12, `fsio` 34, `component` `decl` 16 and `route` 32; `system/lib/wire/test/ordinals_test.py` 5.
- The two greps above, rerun after the change: the only hit left is `format.hpp`'s URL scheme, kept for the reason below.
- clang-tidy with `-p out/bringup.x86` on `loop.cpp`, `path.cpp`, `wire.cpp`, `wire_test.cpp` and `decl.cpp`, with the four libraries' headers in its header filter: no diagnostic.

**Deliberately not done**
- The component URL scheme `fuchsia-boot:///#meta/<name>.cm` stays, in `format.hpp`'s comment and in the tests' string literals: it is the scheme the code parses (`system/service/init/realm.cpp`, `system/component_manifest.py`), so the comment states a fact about ours's data, not a comparison. Renaming the scheme is a code change the user's decision 1 did not include, left for the user to decide: item 2 of `docs/repairs/261004/component.md`.
- The READMEs of `wire`, `vfs`, `loop` and `svc` still name Fuchsia: rule 2 does not touch documents.
- `fsio/path.hpp` and `ours_io.hpp` name POSIX: `path.hpp` says fsio has no POSIX layer whose open() would act on a trailing '/', and `ours_io.hpp` says which standard interface's `d_type` values the enum carries. Both state what ours's own interface is or lacks, not a design ours follows, so they stay.
- `component/test/embed_manifests.py`'s "BSD-style license" line is ours's own license notice, which rule 2 does not touch.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a Loop key carry a generation, and what would go wrong with a key that was the address of the slot, given that ours has no port_cancel?
- Why does the root not re-check a manifest's routing rules when it resolves a component, and what would have to change about who writes the BootFS for that to become unsafe?
- Where does a comparison with another system belong in this repository, and why not in the code comment next to the thing compared?

## Title: the older system libraries' comments say what a thing is and why, without naming another system (2026-10-04)

**Files**
- `system/lib/BUILD.gn` — the root's comment says what the `tests` rollup gathers and how code depends on a library, instead of copying another tree's root
- `system/lib/bootfs/include/bootfs/format.hpp` — PAGE_SIZE's comment states the page rule and its reason without naming the format it shares a constant and a rule with
- `system/lib/bootfs/test/view_test.cpp` — RefusesTwoSizesOnOnePage states the format's sharing rule as the format's own
- `system/lib/elfload/BUILD.gn`, `include/elfload/diagnostics.hpp` — Diagnostics is described by what every refusing elfload call does with it, not by whose shape it copies
- `system/lib/elfload/include/elfload/program.hpp` — check_program() drops a comparison with another kernel's exec path; the reason (judge before anything exists) was already stated
- `system/lib/elfload/src/binder.cpp` — the refusal of DT_REL and DT_RELR says what a complete binder would do, not which foreign function does it
- `system/lib/ffl/include/ffl/fixed.hpp`, `format.hpp`, `mul_shr.hpp`, `saturating.hpp`, `wider.hpp` — expression templates, the format split, the reciprocal-weight shape, rounding and `__int128` are each explained by their own reason
- `system/lib/launch/include/launch/launch.hpp`, `src/launch.cpp`, `src/plan.cpp` — start()'s steps, the stack rule, the initial stack pointer, MappedFile, the image region and the partial-page cut say what they do and why, and the two behaviours that used to be stated as "departures" are stated as behaviours
- `system/lib/libc/BUILD.gn`, `src/string_impl.cpp` — one copy of the string functions for every ring, and memcmp's `unsigned char` rule, each stand on their own reason
- `system/lib/logz4/include/logz4/clock.hpp`, `domain.hpp`, `log.hpp`, `src/log.cpp` — Domain, LogDomain's dynamic scope, the per-image domain list, panic mode, the prefix layout and the clock's unit lose their surveys of other loggers
- `system/lib/ort/include/ort/debug.hpp`, `main.hpp`, `process_args.hpp`, `startup.hpp`, `startup_handles.hpp`, `src/process_args.cpp`, `src/start.cpp` — the runtime's start-up, its processargs decoder and its startup handles say how strict they are and why, rather than how strict another system's decoders are not
- `system/lib/ort/test/process_args_test.cpp` — the file comment and two test comments describe the cases they build instead of whose decoder lets them through
- `system/lib/ustl/include/ustl/collections/intrusive/detail/augment.hpp`, `function/function.hpp`, `rc.hpp` — the augment callbacks, Function's fixed inline size and rc_recycle() drop their foreign models
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
None; this carries out the user's decision 3 of 2026-10-04 evening, the sweep of rule 2 (CLAUDE.md, "Code comments say what a thing is and why, in this codebase's own terms") over the group of older system libraries: `system/lib/bootfs`, `derive`, `eftl`, `elfload`, `ffl`, `hsien`, `launch`, `libc`, `logz4`, `ort`, `ustl` and `system/lib/BUILD.gn`. `derive`, `eftl` and `hsien` needed no change.

**Cause**
A comment that explains this code by pointing at another system's code is wrong in three ways, and the 32 files here showed all three.

- **It makes the reader learn the other system first.** `ort::ProcessArgs::parse()` checked the handle_info bound as `(bytes - info_off) / sizeof(u32) < num_handles`, and its comment said only that one foreign decoder bounds it "exactly this way" and another does not. The actual reason -- count the room after the offset in bytes and only then divide it into entries, because subtracting a word count from a byte count lets a 40-byte message claim five entries at offset 36 -- was there only as a fact about someone else's bug. Stated directly it is one sentence about ours, and it is the sentence a reviewer of this line needs. The same happened in `plan.cpp`, where the one choice ours makes about the bytes before `vaddr` in a segment's first page was written as "one difference" from another loader.
- **It hides which sentences carry information.** "Zircon's zx_debug_write is the same thing for the same reason", "the same shape as Android printk", "The counterpart is Zircon's ffl/saturating_arithmetic.h": each read as a justification, and each carried none -- the reason was already in the sentences before it. Removing the comparison showed which sentences were about ours. Where a comparison did carry a reason ours relies on, the reason was kept and restated in ours's terms: `wider.hpp` still says why `__int128` is acceptable (this tree is clang-only, and a 64-by-64-bit product is what the extension is for), and `launch.hpp` still says that start() starts the process itself and closes the thread, the image's region and the stack's VMO.
- **It goes stale without anyone noticing.** `plan.cpp` cited `vmar-loader.h:274-310` and `load.h:342-359`, `launch.cpp` cited `mapped-vmo-file.cc` by function, and `ffl/fixed.hpp` gave another library's line count ("492 lines"). No build checks either side of such a comparison. The other tree moves, ours changes, and the comment keeps pointing at lines that no longer say what it claims; a comment about ours's own invariant is checked by whoever reads the code next to it.

Where a design does follow another system, that is worth knowing, but it belongs where it is dated and attributed: a change record's Reference and the design documents. This entry's Reference carries everything the comments used to cite, so the comparison material is kept for anyone who wants to read the other code.

**Change**
Only comment text changed; no token of code (checked mechanically, see Verification). The largest rewrite, start()'s comment in `launch.hpp`:

```cpp
// before
/// through that VMO; this reverses that. Steps 3 to 5 -- regions, relocations
/// written into the segments' VMOs, then every mapping made with its final
/// protection, RELRO read-only from the start -- are the shape of Zircon's
/// ld::RemoteDynamicLinker (Allocate, Relocate, Load); the process, the
/// stack and the start are its process_builder's. Two departures from
/// process_builder: start() starts the process itself, so the caller writes
/// the processargs message on its end of |bootstrap| afterwards (the runtime
/// waits for it); and it hands back only the process and its root region,
/// closing the thread, the image's region and the stack's VMO, which
/// process_builder passes on as ThreadSelf, LoadedVmar and StackVmo.

// after
/// through that VMO; this reverses that. Steps 3 to 5 lay out the regions,
/// write the relocations into the segments' VMOs, and only then make every
/// mapping with its final protection, so RELRO is read-only from the start.
/// start() starts the process itself, so the caller writes the processargs
/// message on its end of |bootstrap| afterwards (the runtime waits for it).
/// It hands back only the process and its root region, closing the thread,
/// the image's region and the stack's VMO.
```

The processargs bound in `ort/src/process_args.cpp`, where the reason had been stated only as other decoders' behaviour:

```cpp
// before
    // Zircon's musl processargs_read() bounds the handle_info array exactly
    // this way. The newer ld::ProcessargsBuffer::Valid() subtracts the
    // offset in words from the size in bytes, and so accepts, say, a 40-byte
    // message that claims five handle_infos at offset 36.

// after
    // The room after the offset is counted in bytes and only then divided
    // into entries. Subtracting the offset in words from the size in bytes
    // instead would accept, say, a 40-byte message that claims five
    // handle_infos at offset 36.
```

The rest, by kind:

- Dropped because the sentence was nothing but a comparison: bootfs's page constant and the quoted sharing rule (the rule itself is stated above it), elfload's exec-path comparison, ffl's `FixedFormat` and `saturating_arithmetic.h` lines and the reciprocal-weight sentence in `mul_shr.hpp`, launch's remote-linker read path in step 0, libc's memcmp sources, logz4's `__dyndbg` and `oops_in_progress` lines, ort's `zx_debug_write`, `zx_take_startup_handle()`, `parse_cmdline()` and `__libc_start_main` lines, ustl's `rb_augment_callbacks` line.
- Rewritten to keep a reason the comparison carried: ffl's expression templates (declined, with the reason and the condition to revisit), `mul_shr_round()` (truncating would accept the bias, and the library already rounds), launch's MappedFile (one mapping of the whole VMO, read in place, unmapped by the destructor, read and execute on purpose), plan.cpp's partial-page cut (the Copy starts at `vaddr`, so the bytes before it read as zero, and ELF promises nothing about them), logz4's per-image domain list (listing a phys module's domains would mean registering each module's section as it is loaded), LogDomain's scope (context that travels with the thread of execution, not a name fixed per file at compile time), ort's decoder (strict: every string section is checked, and strings running off the end are refused, not quietly dropped), ort's start-up wait (the starter may start the child before it sends the message), Function's size (fixed, not a parameter, so every `Function<R(Args...)>` is one type), rc_recycle() (what it is for, without the library it was named after).
- `process_args_test.cpp`: the protocol word `0x4150'585d` the test uses as a foreign one is now labelled by its bytes, `"]XPA", not ours`, and the BoundsTheHandleInfoArray case describes the wrong bound it guards against.
- `start.cpp`: one over-long comment line (`The wait ends on READABLE, or on PEER_CLOSED`) was rewrapped in the paragraph that was being rewritten anyway.

The hits were found with the sweep's grep, `(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)`, over the C, C++, assembly, GN and Python files of the group, and with a second grep for foreign names the first cannot see (`zx_`, `fbl`, `fit::`, `sdk/`, `src/lib`, `counterpart`, `process_builder`, `elfldltl`, `musl`, `glibc`, `Android`, `GLib`, `log4j`, `Rust`, `Boost`), which found the comments in `binder.cpp` (glibc) and `clock.hpp` (Android).

**Reference**
The comparisons the comments carried, kept here for the reader who wants to compare:

- system/lib root: Zircon's `system/ulib` root is a test-discovery rollup in the same way.
- bootfs: the page is Zircon's `ZBI_BOOTFS_PAGE_SIZE` (`lib/zbi-format/internal/bootfs.h`), whose format states the same sharing rule: "Each range of pages can be used for multiple files, but all those files must have the same offset and size, that is, no partial sharing of ranges."
- elfload: Diagnostics follows Zircon elfldltl, which takes a Diagnostics object in every call that can fail (`lib/elfldltl/diagnostics.h`). check_program() before any process exists matches Linux `load_elf_binary()`, whose header checks all come before `begin_new_exec()`, the point after which the old image is gone. A full binder walks REL, RELA and RELR as glibc `elf_dynamic_do_Rel()` does.
- ffl: Zircon's ffl uses expression templates (`ffl/expression.h`, 492 lines) so the assignment target picks the intermediate precision; it splits the layout out as `FixedFormat`; the saturating helpers match `ffl/saturating_arithmetic.h`; its 64-bit path, like Linux `mul_u64_u64_shr()`, uses `__int128`. `mul_shr()` has the shape of Linux's `sched_prio_to_wmult[]` (2^32/weight) scaled by `__calc_delta()` through `mul_u64_u32_shr()`; `__calc_delta()` truncates where ffl rounds.
- launch: steps 3 to 5 have the shape of Zircon's `ld::RemoteDynamicLinker` (Allocate, Relocate, Load); the process, the stack and the start follow `process_builder`, which passes the thread, the loaded VMAR and the stack VMO on as `ThreadSelf`, `LoadedVmar` and `StackVmo` and does not start the process itself; `process_builder` reads headers with `vmo_read`, the remote dynamic linker through a read-only mapping (`elfldltl::MappedVmoFile`, `src/lib/elfldltl/mapped-vmo-file.cc`, `Init()` and `~MappedVmoFile()`, used by `ld::RemoteDecodedModule`), whose destructor ignores the unmap's status in a release build. The stack size rule (PT_GNU_STACK rounded up to a page, or a default) is `process_builder`'s with a smaller default; the initial stack pointer matches `compute_initial_stack_pointer()`; per-step failure reports match `VmarLoader`'s for each failed map and clone; the image region's SPECIFIC permission matches `VmarLoader::AllocateVmar()`. The partial-page cut is `elfldltl::VmarLoader`'s (`vmar-loader.h:274-310`, `PartialPagePolicy::kCopyInProcess`); Zircon rounds the segment's offset down first (`load.h:342-359`), so its bytes before `vaddr` show the file's where ours read as zero.
- libc: Zircon keeps two copies of the string functions, `//zircon/kernel/lib/libc/string` and `//sdk/lib/c/string`. Linux `lib/string.c` `memcmp()` and Zircon `kernel/lib/libc/string/memcmp.c` read through `unsigned char` the same way.
- logz4: the domain section follows Linux dynamic_debug's `__dyndbg` descriptors, which `ddebug_add_module()` registers per module at load time. A Domain is GLib's log domain (`G_LOG_DOMAIN`), the Rust `log` crate's target (`RUST_LOG=kshell=debug`), and an Android or Fuchsia syslog tag; Linux has `pr_fmt` per file and dynamic_debug per module. LogDomain's dynamic scope is unlike Linux `pr_fmt` and GLib `G_LOG_DOMAIN` (fixed per file at compile time) and like Rust tracing's `span.enter()` guard and log4j's `ThreadContext`. Panic mode makes the promise Linux `oops_in_progress`, set by `bust_spinlocks(1)` in `panic()`, makes to its console drivers. The prefix fields (time, cpu, level) are in the order of Android's printk, and the domain sits where Linux `pr_fmt` puts its prefix. Android's logger also stores a raw time quantity and leaves the unit to the clock's supplier.
- ort: `ort::debug_write` is Zircon's `zx_debug_write` for the same reason. Zircon programs need no `<ort/main.hpp>` because they are compiled hosted, where `crt1.S` names `main` unmangled. Zircon has two processargs decoders: musl `processargs_read()`, which never reads the names section, answers `ZX_ERR_INVALID_ARGS` for a malformed message and bounds the handle_info array as ours does; and `ld::ProcessargsBuffer`, which accepts strings that run off the end (reporting fewer), bounds the handle_info array by subtracting words from bytes in `Valid()`, and waits for the message first because the parent "might have started this process before sending its bootstrap message". Zircon's start-up code queries the message size before reading and closes the bootstrap channel after; the old `__libc_start_main` kept argv in a variable-length array on the start-up frame. Zircon userboot's `parse_cmdline()` (`main.rs`) lets a later option override an earlier one. `ort::take_startup_handle()` is `zx_take_startup_handle()`, which clears its slot, and Zircon also leaves unclaimed handles open. The test's golden message is `process_builder`'s (`src/lib/process_builder/src/process_args.rs`, the `build_and_write_message` test), and `0x4150585d` is Zircon's `ZX_PROCARGS_PROTOCOL`.
- ustl: the augment callbacks are modelled on Linux `struct rb_augment_callbacks`. Function is move-only like Zircon's `fit::function`, and is `fit::inline_function<Signature, inline_target_size>` (`sdk/lib/fit/include/lib/fit/function.h`) with the size fixed rather than a parameter. `rc_recycle()` is fbl's `fbl_recycle()`, and deferring the next link is Zircon's `SafeDeleter`.

**Verification**
- No code changed: each of the 32 touched C++ and GN files, with its comments stripped and its whitespace collapsed, is the same before and after (a scratch script that strips `//` and `/* */` comments, skipping string and character literals and digit separators, and `#` comments in GN).
- `scripts/format_cpp.py --check` on the 29 C++ files and `scripts/format_gn.py --check` on the three GN files: clean.
- Build and boot, `boot_check.py` on `out/bringup.x86`: build ok; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines).
- Host tests of the touched libraries, all passed: bootfs `view` 9, ffl `fixed` 7, launch `plan` 36, `program` 8 and `process_launcher` 15, libc `string` 12, ort `process_args` 16, and `logz4-test`, ustl `rc-test`, `rc-doc-examples-test`, `function-test` and `rbtree-augment-test` (exit status 0). elfload has no host test of its own; it is built into the kernel and `launch`, both covered above.
- clang-tidy with `-p out/bringup.x86` on the nine touched `.cpp` files: no diagnostic on a changed line; the only diagnostics are in untouched headers (`bootfs/view.hpp`, `builder.hpp`) and in googletest.
- The two greps above, rerun after the change: the hits left are the ones listed under "Deliberately not done", each with its reason.

**Deliberately not done**
- `eftl/include/eftl/elf/note.hpp:88` keeps "GNU/Linux note sections (e.g. .note.gnu.build-id) are always 4-byte aligned regardless of ELF class": it states what the ELF files the parser reads actually contain, the GNU ELF ABI's note layout, not a design ours follows.
- `libc/BUILD.gn:57` keeps "(Linux on arm64)": a fact about a host platform the tests may run on, where `char` is unsigned.
- `ort/test/process_args_test.cpp:26` keeps the golden message's source: the bytes are copied from another project's test, so naming it is an attribution, and the test that uses it, `WritesZirconsLayoutByteForByte`, is an identifier, which rule 2 does not touch and this sweep may not change. Whether ours should keep a byte-for-byte compatibility test with another system's processargs layout at all is a design question for the user, not a comment question.
- `libc/include/errno.h:53` keeps "(glibc uses the same names)": it says which C library ABI fixes the names `__errno_location` and `errno`, which is why the naming check is switched off there, not a design comparison.
- Rust and Boost in comments stay: ustl's API takes Rust's vocabulary (`Box`, `Rc`, `Arc`, `Some`, `Box::from_raw`, `Box::leak`) and its intrusive containers are ports of Boost.Intrusive, so `box.hpp`, `rc.hpp:728-730`, `option.hpp:261,348`, `rc_test.cpp`, `box_test.cpp`, `slist.hpp`, `circular_slist_algorithms.hpp`, `parent_from_member.hpp`, `key_node_ptr_compare.hpp`, `btree_node_algorithms.hpp` and `logz4/domain.hpp:122` (`RUST_LOG`) name them. Rule 2 names operating systems; whether language libraries fall under its last sentence is the user's call, recorded here rather than decided quietly. Where one of them sat inside a sentence that was being removed for naming Zircon, Fuchsia, Linux or Android, it went with that sentence (`logz4/domain.hpp`'s survey of loggers).
- READMEs in the group are documents, which rule 2 does not touch; `ustl/include/ustl/util/enum_sequence.hpp` carries another project's (GPL) header and the third-party trees are outside the group, so none of them were read for edits.
- No kernel `*.test` command was run: no code changed, and the boot's own self-checks passed on both images.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `ProcessArgs::parse()` divide the room after `handle_info_off` into entries only after counting it in bytes, and which message would a word-minus-byte bound let through?
- In a writable segment whose file data ends inside a page, why must the bytes after the file data read as zero, and why do the bytes before `vaddr` in its first page not matter?
- Where does a comparison with another system belong in this repository, and what goes wrong when it sits in the code comment instead?

## Title: the public headers, init, the echo example and the system build files describe themselves without naming other systems (2026-10-04)

**Files**
- `system/public/ours/exception.hpp`, `interrupt.hpp`, `object_info.hpp`, `object_property.hpp`, `port.hpp`, `process_args.hpp`, `rights.hpp`, `signals.hpp`, `status.hpp`, `syscall.hpp`, `syscall/resource.hpp`, `types.hpp`, `vdso.hpp` — every "the values are Zircon's ZX_*" becomes "the values are ABI and never change"; foreign macro, type and function names given as an equivalent (`zx_port_packet_t`, `ZX_HANDLE_INVALID`, `PA_HND()` and the like) are dropped; where a value is skipped or reserved, the comment now says which bit or number is kept and for what, in this ABI's terms
- `system/public/ours/syscalls.gni` — three `doc` strings lose a trailing "as in Zircon"; these strings are emitted as the `///` comments of the generated `<ours/vdso_decls.inc>`, so they are comment text in source form
- `system/service/init/realm.hpp`, `system/service/init/include/init/autorun.hpp` — the instance states, the boot resolver, the eager-start loop and the autorun option are described by what they do here, without component_manager source paths or console-launcher function names
- `examples/echo/BUILD.gn`, `examples/echo/echo_server.cpp`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp` — the echo provider and its protocol say why each method exists (EchoString proves a route, Stop lets the checking client see the provider go) instead of which routing example they were copied from
- `system/BUILD.gn`, `system/public/BUILD.gn`, `system/component_manifest.gni`, `system/user_executable.gni` — the availability stub, the public headers' target, the manifest template and the program install path are described in this tree's terms

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of these comments was sent to another code base to learn what a value or a rule means here, and a comment's claim ("the values are Zircon's") could not be checked against anything in this tree.

**Cause**
The comment rule adopted on 2026-10-04 (CLAUDE.md, "Code comments say what a thing is and why, in this codebase's own terms") puts every comparison with another system in the change record's Reference field and in the design documents, never in code. These files were written before the rule and leaned on comparison as explanation, which hides the real reason in three ways. First, "the values are Zircon's ZX_EXCP_*" states where a number came from, not what makes it safe to rely on: the property that matters to a program is that the number is ABI and never changes, and that is what the comments now say. Second, a comparison often carried the one piece of real information in a sentence, such as "Zircon's third flag, DEBUGGER_ATTACHED on bit 2, is left out", whose content is that bit 2 is reserved for a debugger flag; dropping the sentence whole would have lost that bit's reservation, so each such sentence was rewritten to keep it ("Bit 2 is kept for a third, a debugger being attached"). Third, "as in Zircon" was sometimes the only stated reason for a rule, as with "a child that fails to start is logged and the next one still starts, as Fuchsia logs a failed eager child"; the rewrite gives the reason in this system's terms (one broken child does not keep its siblings down), which is what a reader needs in order to judge whether the rule still holds when the code changes.

**Change**
Comment text only; no code, no identifier and no string a program reads changed. A check that every changed line of the diff is a comment line (or one of the three `doc` strings above) passes. Typical rewrites:

```cpp
// Before
/// What a port packet reports. The values are Zircon's ZX_PKT_TYPE_*
/// (zircon/syscalls/port.h).
// After
/// What a port packet reports. The values are ABI and never change.

// Before
/// The state bits of an InfoProcess, at Zircon's positions
/// (ZX_INFO_PROCESS_FLAG_*). Zircon's third, DEBUGGER_ATTACHED on bit 2, is
/// left out: there is no debugger to attach.
// After
/// The state bits of an InfoProcess. Bit 2 is kept for a third, a debugger
/// being attached, left out while there is no debugger to attach.

// Before
/// Zircon counts the same packets, port_queue's and no others
/// (kMaxAllocatedPacketCountPerPort in object/port_dispatcher.cc, checked in
/// QueuePacketLocked()), but allows 4096, and a caller past the limit gets a
/// policy exception rather than a status. Here it is a status the caller can
/// back off on.
// After
/// Only port_queue's packets count, none the kernel queues. A caller past
/// the limit gets a status it can back off on, not an exception.
```

Left as they are, because rule 2 does not reach them: the `fuchsia-boot:///#` URL scheme, which is a value the build writes into every compiled manifest and the root parses (decision D26), in `system/component_manifest.py`, `system/service/init/realm.cpp`, `system/service/init/realm.hpp`, `system/test_component_manifest.py`, and quoted as that value in the comment of `system/component_manifest.gni`; the BSD license notices; `musl` in the `MAX_NAME_LEN` comment of `syscall.hpp`, which is a fact about the C library header a program may include, not a design reference.

**Reference**
The values these comments used to attribute are Zircon's, from `zircon/system/public/zircon/` in the Fuchsia tree: `ZX_EXCP_*` and `zx_exception_info_t` (`syscalls/exception.h`), `ZX_INTERRUPT_*` (`types.h`), `ZX_INFO_*`, `ZX_INFO_PROCESS_FLAG_*`, `zx_info_process_t` and `ZX_PROP_*` (`syscalls/object.h`), `ZX_PKT_TYPE_*`, `zx_packet_signal_t`, `zx_port_packet_t` (`syscalls/port.h`), `zx_proc_args` and `PA_*` (`processargs.h`), `ZX_RIGHT_*` (`rights.h`), `ZX_OBJECT_*` and `ZX_OBJ_TYPE_*` (`types.h`), `ZX_RSRC_KIND_*` and `ZX_RSRC_SYSTEM_*_BASE` (`syscalls/resource.h`), `ZX_VM_*` and `ZX_VMO_CHILD_*` (`types.h`), `ZX_TASK_RETCODE_*` (`syscalls/object.h`). The numbers kept out of use are Zircon's too: `RsrcKind` 3 is its removed root kind and 4 is `ZX_RSRC_KIND_SMC`, the ARM-only secure monitor kind; System base 0 is `ZX_RSRC_SYSTEM_HYPERVISOR_BASE`; `RsrcFlag` 0x00010000 is `ZX_RSRC_FLAG_EXCLUSIVE` (all `syscalls/resource.h`); `VmoChildOptions` bit 1 was `ZX_VMO_CHILD_NON_RESIZEABLE`, since removed (`types.h`); `ExceptionChannelOptions` 1 is `ZX_EXCEPTION_CHANNEL_DEBUGGER` (`syscalls/exception.h`), and `ExceptionState` 2 is `ZX_EXCEPTION_STATE_THREAD_EXIT` (`syscalls/object.h`); `PortCreateOptions` bit 0 is `ZX_PORT_BIND_TO_INTERRUPT` and `WaitAsyncOptions` bits 0 and 1 are `ZX_WAIT_ASYNC_TIMESTAMP` and `ZX_WAIT_ASYNC_EDGE` (`syscalls/port.h`). The port limit is `kMaxAllocatedPacketCountPerPort` in `zircon/kernel/object/port_dispatcher.cc`, which is 4096 and answers a caller past it with a policy exception, where ours allows 256 and answers ShouldWait; Zircon also counts only armed observers against its observer limit, where `PORT_MAX_OBSERVERS` counts queued signal packets too. A channel message may be 64 KiB in Zircon (`ZX_CHANNEL_MAX_MSG_BYTES`), against 4 KiB here. The component framework's states and eager-start loop follow `src/sys/component_manager/src/model/component/instance.rs` and `mod.rs`; the root URL follows `startup.rs`; the autorun option follows `zircon.autorun.boot` and `LaunchAutorun()` in console-launcher; the echo example follows `examples/components/routing/cpp/echo_server`. Recorded here so that the comparison is not lost now that the code no longer carries it.

**Verification**
`scripts/format_cpp.py --check` and `scripts/format_gn.py --check` clean on every changed file; clang-tidy on `examples/echo/echo_server.cpp` reports nothing in the swept files. boot_check: build ok, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 0 failed). Host tests: `ours_examples_echo` 7/7, init's `autorun` 5/5, `system/test_component_manifest.py` 48 OK. A grep over the swept directories for `zircon|fuchsia|linux|\blk\b` finds only the `fuchsia-boot:` URL value and its quotation.

**Deliberately not done**
No identifier was renamed, including the `fuchsia-boot:///#` URL scheme, which is a recorded decision (D26) and a value, not a comment. Comments elsewhere in `system/lib`, `kernel` and the build are other groups' sweeps.

**How it was found**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over the C, C++, assembly, linker-script, GN and Python files of `system/public`, `system/service`, `examples` and the files directly in `system/`, then each hit read in context, together with neighbouring lines that named a foreign artefact without the word itself (`ZX_*`, `zx_*`, `CML`, `cmc`, `elfldltl`, `cm/...` paths).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is "the values are ABI and never change" more useful to a program's author than "the values are Zircon's"?
2. When a value is reserved because another system uses it, what must the comment still say once the other system's name is gone?
3. Why does the `fuchsia-boot:` URL stay in code even under the comment rule?

## Title: the build files, tools, tests and scripts describe themselves without naming other systems (2026-10-04)

**Files**
- `build/abi/gen_syscalls.py`, `build/abi/verify_vdso_abi.py` — the module docstrings explain why the syscall description is a GN scope list, why a call takes at most eight arguments, and what the vDSO ABI check catches, without the generator and BUILD rule they were compared with
- `build/config/BUILD.gn`, `build/config/BUILDCONFIG.gn`, `build/config/arm/BUILD.gn`, `build/config/clang/include/__config_site`, `build/config/compiler/clang.gni`, `build/config/compiler/compiler.gni`, `build/config/hermetic/BUILD.gn`, `build/config/ours/variants.gni` — the config layering, what BUILDCONFIG does, the default toolchain carrying target identity, the host-toolchain flag, the ARMv8-A baseline, where the libc++ site configuration sits, what keys a toolchain rebuild, the compiler switch and the variant registry are each described by what they are and why
- `build/cpp/cpp_interface.gni` — a third reason that was only "another build does the same" is dropped; the two real reasons stay
- `build/dist/convert_fini_to_manifest.py`, `build/dist/convert_manifest_to_fini.py`, `build/dist/distribution_manifest.gni`, `build/dist/distribution_manifest_copy.gni`, `build/dist/distribution_manifest_unittests.py`, `build/dist/fini_manifest.gni`, `build/dist/renamed_binary.gni`, `build/python/modules/distribution_manifest/BUILD.gn`, `build/python/modules/distribution_manifest/__init__.py`, `build/python/modules/distribution_manifest/distribution_manifest.py` — NOTE(ours) comments that described a dropped upstream variable or wrapper now say what this build does (a native depfile, no action tracer, no package concept); the FINI converters' docstrings name the direction they convert in, which one of them had backwards
- `build/obi/obi.gni`, `build/obi/obi_input.gni`, `build/obi/resource.gni` — the obi() notes list what obi() produces and how it nests, and resource()'s usage notes name obi_input() and fini_manifest() as the collecting targets instead of package and component templates this tree does not have
- `build/product/ours_args.gni`, `build/product/product.gni`, `tools/set.py` — the `ours` argument bundle, product assembly, and set.py's `--with`, `--args` and `--autorun` are described by what they do; the "counterpart of" and "equivalent of" comparisons with another build's commands and boot option are gone
- `build/switch/BUILD.gn`, `build/switch/kernel_switchset.gni`, `build/switch/phys_switchset.gni` — the named debug-level configs and the 0..2 range are described without calling them shaped after, or compatible with, another kernel
- `build/toolchain/basic_toolchain.gni`, `c_utils.gni`, `concurrent_jobs.gni`, `default_concurrent_jobs.gni`, `hermetic_source_set.gni`, `ifs_shared_library.gni`, `ours/ours_toolchain.gni`, `restat.gni`, `restat_cacher.py`, `toolchain_environment.gni`, `toolchain_tags.gni`, `toolchain_utils_action.py`, `variant.gni`, `variant_toolchain_suite.gni` — each local-adaptation note states the property of this build (local-only, no action tracer, never the gold linker, `tags` or `toolchain_tags`, deps optional) instead of listing what the upstream file has; the tag descriptions say "toolchains", "kernel" and "the compiler ABI" in this tree's terms
- `scripts/lldb_ours.py` — `CpuLocal::CPU_OFFSETS` is described as the table of every CPU's offset
- `tests/dist-smoke/BUILD.gn`, `tests/hermetic-smoke/BUILD.gn`, `tools/BUILD.gn` — the nested-container smoke, the explicit link_output_path metadata and the unavailable tools-zither seam in this tree's terms
- `docs/repairs/261004/build-dist.md` — new: the dead `zbi_input_barrier` metadata key and the manifest-format document path, which the sweep left alone because they are not comparisons

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of these comments was sent to another code base to learn what a build rule, a tag or a command-line option means here, and two of the comments said something about this tree that was not true.

**Cause**
The comment rule adopted on 2026-10-04 (CLAUDE.md, "Code comments say what a thing is and why, in this codebase's own terms") moves every comparison with another system into the change record's Reference field and the design documents. The build was ported template by template from another tree, and its comments were written as a diff against that tree: "Fuchsia sets check_for_output_dir_leaks = false here; that variable belongs to its BUILDCONFIG action() wrapper and is unknown to the Ours build, so it is dropped". Such a comment explains the absence of something the reader of this tree has never seen, and the fact the reader needs (this build does not trace actions for undeclared writes, and the script writes only its declared outputs) is buried in the middle of it. The rewrites lead with that fact.

Three kinds of sentence needed care. First, a comparison was sometimes the only stated reason: "Zircon draws the line at eight for the same reason" is the whole justification for `MAX_ARGS = 8` if the preceding words are not read closely; the rewrite says the reason itself, that every argument then arrives in a register and the kernel never reads a ninth through user_copy. Second, a comparison often hid a concrete limit: "obit has neither capability yet" (a .json content listing and a depfile) and "compressed = true maps to lz4 rather than the other build's default zstd" carry the limits of obit, so each limit was kept and only the comparison dropped. Third, rewriting forced each claim to be checked against this tree, and two were wrong while a third gave no reason at all. `toolchain_tags.gni` said the two OS-selection tags "are kept as definitions for registry completeness"; neither is defined anywhere, so the comment now says they are not defined and why. `convert_fini_to_manifest.py`'s docstring, which argparse prints as its `--help`, said it converted a distribution manifest to FINI, the opposite of what it does. And `distribution_manifest_unittests.py` blamed a missing `import sys` on "an upstream bug", which tells the reader nothing about why the import must be explicit here (sys.path is used before any module could import it transitively).

The PROVENANCE blocks were treated as attribution and kept, including the sentence after the copyright that lists what differs from the original: that list is how a BSD-style port records its modifications, and it is the one place in a file where naming the origin is the point. Where a rewrite changed a docstring in a file whose PROVENANCE said "only the shebang and this header differ", the PROVENANCE sentence was extended to say the docstring differs too, so the attribution stays true.

**Change**
Comment text only. Every changed file was checked mechanically: Python files have an identical AST once docstrings are removed, and GN and C files have an identical token stream once comments are removed. Docstrings count as comment text; four of them (`gen_syscalls.py`, `verify_vdso_abi.py`, `restat_cacher.py` and the two FINI converters) are also printed by `--help`, so that output changed with them. Typical rewrites:

```text
# Before (build/dist/fini_manifest.gni)
    # NOTE(ours): Fuchsia sets check_for_output_dir_leaks = false here; that
    # variable belongs to its BUILDCONFIG action() wrapper and is unknown to
    # the Ours build, so it is dropped (the script only writes its declared
    # outputs).
# After
    # NOTE(ours): this build does not trace actions for undeclared writes;
    # the script only writes its declared outputs.

# Before (build/abi/gen_syscalls.py)
# kernel would then have to read through user_copy -- Zircon draws the line at
# eight for the same reason.
# After
# kernel would then have to read through user_copy, so the line is drawn at
# eight: every argument arrives in a register.

# Before (tools/set.py)
--autorun is the `zircon.autorun.boot` counterpart: a command,
`<BootFS name>[+arg...]`, that init runs once the shell is up. The name is
the program's whole BootFS path, `bin/foo` for //examples/foo, as Zircon's
takes `/boot/bin/foo`.
# After
--autorun takes a command, `<BootFS name>[+arg...]`, that init runs once the
shell is up; it reaches the kernel command line as `ours.autorun.boot=`. The
name is the program's whole BootFS path, `bin/foo` for //examples/foo.

# Before (build/toolchain/toolchain_tags.gni)
  # This indicates a toolchain does not provide the Fuchsia Compiler ABI.
# After
  # This indicates a toolchain does not provide the compiler ABI: the
  # thread-pointer-relative slots (such as the stack-protector guard) that
  # compiler-generated and instrumentation code may read.
```

Left as they are, because rule 2 does not reach them:
- PROVENANCE attributions and their lists of local changes, in about fifty files under `build/`, and the attribution inside `restat_cacher.py`'s docstring.
- Files that carry another project's copyright header: `build/toolchain/variant_tags.gni` (its "Fuchsia Compiler ABI", "Zircon specific" and `fuchsia.debugdata` comments) and `build/config/hermetic-code-blob.ld`.
- Facts about the host platform: `build/config/compiler/clang.gni` (the container's Linux build, llvm-22's Linux symlinks) and `build/toolchain/get_macos_sdk_path.py` (apt's llvm-22 resolving against the Linux system root).
- Code and data: `sys.platform.startswith("linux")` in `build/toolchain/get_concurrent_jobs.py`, the schema `id` URL in `build/dist/distribution_schema.json`, the identifiers `lk_debug_level`, `zbi_input_barrier` and `tools-zither`, and two argparse strings in `tools/set.py`, its `description` ("fx set equivalent") and the `--autorun` help ("like zircon.autorun.boot"), which are program output and outside a comment-only change.
- Upstream bug links (`https://fxbug.dev/...`) in `build/obi/resource.gni`, `build/toolchain/hermetic_source_set.gni`, `build/toolchain/copy.py`, `build/toolchain/copy_command.sh` and `build/toolchain/verify_depfile.py`: each cites the evidence for a fact about LLD, macOS mtimes or instrumented builds, not a design to compare with; `copy.py` and `verify_depfile.py` are also verbatim ports.
- The `zbi()`/`zbi_test()`/`zbi_input()` comments beside `zbi_input_barrier` and the manifest-format document path: they claim a consumer and a document that do not exist here, which is a code decision, so they are `docs/repairs/261004/build-dist.md` item 1.

**Reference**
The comparisons these comments carried, recorded here so they are not lost: `fx set <product>.<board>`, `fx set --with` and `fx set --args`; Zircon's `zircon.autorun.boot`, run by console-launcher after the shell starts; zither (and kazoo before it), which generates Zircon's syscall bindings from the zx FIDL library, and Zircon's own cap of eight syscall arguments; the `verify-abi` action in `//zircon/kernel/lib/userabi/vdso/BUILD.gn`, which checks `libzircon.so` against the checked-in golden `//src/zircon/lib/zircon/zircon.ifs`; Fuchsia's `//build/config/BUILDCONFIG.gn` (`is_fuchsia_host`, set_defaults rather than wrapper templates, the action() wrapper with `hermetic_inputs_file`, `check_for_output_dir_leaks` and `hermetic_action_ignored_prefixes` for the action tracer); `zbi()` and `zbi_input()` in `//build/zbi/zbi.gni` (a .json listing and a tool depfile beside every .zbi, zstd by default, self-registration through `zbi_input_args`); `fuchsia_package()`, `fuchsia_component()` and `fuchsia_test_component_manifest()`; Zircon's `known_variants`; Zircon KTL's libc++ `__config_site`; the CIPD instance ID that keys Fuchsia's toolchain rebuilds; `zircon_toolchain.gni`'s `gcc` variant tag; `clang_toolchain_info` in Fuchsia's `basic_toolchain.gni`; RBE branches in `concurrent_jobs.gni`, `restat.gni` and `restat_cacher.py`; the bash `toolchain_utils_action.sh`; `compiler_config()` behind Fuchsia's `variant()`; `is_pic_default` and the `fuchsia-only`/`fuchsia-excluded` tag merge in `variant_toolchain_suite.gni`; the gold-linker branch of `hermetic_source_set.gni`; `exe.unstripped`; Fuchsia products, product bundles and `images.json`; Linux's `__per_cpu_offset[]`, the counterpart of `CpuLocal::CPU_OFFSETS`.

**Verification**
A comment-only check over every changed file (AST without docstrings for Python, comment-free token streams for GN and C) reports no code change. `python3 scripts/format_gn.py --check` on the changed GN files: clean, or "unsafe" where gn's sort would change the file beyond layout, which the hook does not count against a commit. `python3 -m py_compile` on every changed Python file; `python3 build/dist/distribution_manifest_unittests.py`: 14 tests OK; `python3 tools/set.py --list` runs. boot_check: build ok, dbg PASS (29 passed, 2 skipped, 0 failed), rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required). A grep over `build`, `tools`, `tests`, `scripts`, `benches`, `BUILD.gn` and `.gn` for `zircon|fuchsia|linux|\blk\b` in comment lines finds only the hits listed above as left alone.

**Deliberately not done**
No identifier, string a program prints, or metadata key was changed, including `lk_debug_level`, `zbi_input_barrier` and set.py's argparse text. The PROVENANCE blocks keep their attribution and their list of local changes. Documents (READMEs, `docs/`, `.agents/`) are outside the rule. Comments elsewhere (`kernel`, `system`, `products`, `examples`) are other groups' sweeps.

**How it was found**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over the C, C++, assembly, linker-script, GN and Python files of the swept directories, then a second grep without the comment-marker prefix (for docstring and continuation lines) and one for foreign artefacts named without the system's name (`fx set`, `fxbug`, `zbi`, `zither`, `component_manager`); each hit was read in context, and each rewritten claim was checked against the code it describes.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does "this build does not trace actions for undeclared writes" serve a reader better than "Fuchsia sets check_for_output_dir_leaks = false here"?
2. Why does a PROVENANCE block keep naming the project a file came from, when a comment two lines below it may not?
3. Rewriting a comparison into a plain claim turned up two false statements; what made them easy to miss while they were phrased as comparisons?

## Title: review of the build and tools sweep: paragraphs the deletions left ragged, three claims made exact, and set.py's help text recorded (2026-10-04)

**Files**
- `build/config/BUILDCONFIG.gn`, `build/dist/distribution_manifest.gni`, `build/dist/renamed_binary.gni`, `build/product/product.gni`, `build/switch/BUILD.gn`, `build/switch/kernel_switchset.gni`, `build/switch/phys_switchset.gni`, `build/toolchain/hermetic_source_set.gni`, `build/toolchain/toolchain_tags.gni`, `tests/dist-smoke/BUILD.gn`, `tools/set.py` — comment paragraphs with a short line in the middle, where the sweep cut a phrase, are refilled; no wording changes
- `build/toolchain/c_utils.gni` — the `script` parameter's sentence is reordered so that what the standard script does comes before why it is Python, instead of a parenthesis that split `script_executable = "python3"` across lines
- `build/obi/resource.gni` — "The bin/ and test/ destination guard is kept" said kept without saying from what; it now says what the guard does, including the `allow_binary_output_dir` escape the three asserts accept
- `build/config/ours/variants.gni` — "a toolchain suite instantiates a toolchain only for the entries that are selected" left out `enable_variants`, which instantiates a variant no selector names; both are now named
- `build/dist/distribution_manifest_unittests.py` — the reason given for `import sys` was wrong and is replaced
- `build/abi/verify_vdso_abi.py` — "it can never fall behind the description it is checked against" read as if libours.ifs were checked against the description, when it is the vDSO that is checked against libours.ifs; the sentence now says what generating it buys
- `docs/repairs/261004/tools-set.md` — new: set.py's two argparse strings that still compare it with `fx set` and `zircon.autorun.boot`

**Symptom**
None at run time. A reader met a three-word line in the middle of a paragraph in twelve places, and two comments stated a reason or a limit that the code does not bear out.

**Cause**
- Deleting a clause from a wrapped comment leaves the line it was on short, and neither `gn format` nor clang-format ever joins a short line with the next one, so every such deletion has to be refilled by hand. The sweep's own checks (comment-only token comparison, `format_gn.py --check`) cannot see this, because the tokens and the layout gn cares about are both unchanged.
- The `import sys` comment said the import must be explicit "because sys.path is used below before any module could import it transitively". No transitive import could ever have helped: `import sys` inside another module binds the name `sys` in that module's namespace, not in this one, so a module that uses the name `sys` must import it itself whatever else has been imported and whenever. The sentence offered an ordering argument for what is a scoping fact.
- `variants.gni`'s "only for the entries that are selected" is false for `enable_variants`: `variant_toolchain_suite()` sets `_variant_used` for every name in `enable_variants` without consulting `ours_variant_selectors`, so a suite can instantiate a variant that no `select_variant` entry names.

**Change**

```text
# before (build/dist/distribution_manifest_unittests.py)
import sys  # NOTE(ours): imported explicitly, because sys.path is used below
# before any module could import it transitively.
# after
import sys  # NOTE(ours): sys.path is used below, and another module's
# import of sys never binds the name in this one.

# before (build/config/ours/variants.gni)
# instantiates a toolchain only for the entries that are selected.
# after
# instantiates a toolchain only for an entry that select_variant or the
# suite's enable_variants picks.
```

The two argparse strings in `tools/set.py` are not changed here: this sweep is held to comment text, and changing what a program prints is a code change, so they are repair item 1 in `docs/repairs/261004/tools-set.md`.

**Reference**
None, this project's own conventions: CLAUDE.md's comment rule of 2026-10-04 and Python's import semantics (an `import` statement binds a name in the namespace of the module that executes it).

**Verification**
The same comment-only check as the first entry (Python AST without docstrings, GN and C token streams without comments) over every file of the branch against `mvp/integration`: no code change. `python3 scripts/format_gn.py --check` on the changed GN files: clean, except `BUILDCONFIG.gn`, which was already "unsafe" before this review. `py_compile` on the changed Python files; `python3 build/dist/distribution_manifest_unittests.py`: 14 tests OK; `python3 tools/set.py --list` runs. boot_check: build ok, dbg PASS, rel PASS.

**Deliberately not done**
`build/toolchain/variant_tags.gni` keeps "Fuchsia Compiler ABI", "Zircon specific toolchains" and "fuchsia.debugdata": the file carries another project's copyright header, and the other sweeps of this run leave such files as they are. Its comments are stale for this tree as well as foreign, so a later change that takes the file over as this project's own should rewrite them together with the dropped `fuchsia-only`, `uses-shlibs` and `cfi` tags it never recorded.

**How it was found**
Reviewing the sweep's diff line by line against the code each rewritten claim describes (`variant_toolchain_suite.gni`'s `_variant_used`, `resource.gni`'s asserts, `kernel/lib/userabi/vdso/BUILD.gn`'s verify action), and a script that flags a changed comment line ending short of the fill width while the next line's first word would have fit.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can a comment-only token check and a layout check both pass on a paragraph that a reader still sees as broken?
2. A test module calls `sys.path.append()` after `import os`, and `os` imports `sys` itself. Why does the test still need its own `import sys`?
3. `select_variant` is empty and a suite passes `enable_variants = [ "lto" ]`. Is `kernel-x86-lto` defined, and which line of `variant_toolchain_suite.gni` decides it?

## Title: the sampler's comments and faults.cpp's say what a thing is and why, without naming another kernel (2026-10-04)

**Files**
- `kernel/include/ours/sampler/mod.hpp` — the namespace comment says what the per-CPU timer buys (nothing armed, nothing paid, while no session runs) instead of naming another kernel's sampler class and source path; `MIN_PERIOD` keeps its own reason and drops the foreign constant
- `kernel/kernel/sampler/sampler.cpp` — the control mutex loses its foreign name; `stop()`'s closing paragraph states the condition its IPI barrier depends on instead of describing how another kernel waits for writers
- `kernel/arch/x86/kernel/faults.cpp` — the twelve hits the `kernel/arch` sweep had to leave: the user-exception mapping, the single-step `#DB` kill, the NMI lock rule, `deliver_user_exception()`, `arch_dump_exception_context()`, the syscall-exit signal check, the `%gs` instrumentation note, the paranoid handler and the epilogue's order
- `docs/repairs/261004/comment-rule.md` — items 1 (`mod.hpp`) and 3 (`faults.cpp`) deleted; the `fair.hpp` item, which stays the user's, is now item 1

**Symptom**
Latent, cannot be triggered today: no behaviour changes. These were the last hits of the rule's grep outside `fair.hpp`, left by the sweeps because the prof session had the files open (`docs/repairs/261004/comment-rule.md` items 1 and 3). That session's change landed as `a52bd7ea`.

**Cause**
The same as the sweeps' entries above: a comparison with another kernel sends the reader to that kernel's code for the reason, and goes stale silently. Two of these hid a statement about this code, and those were rewritten rather than deleted.

- `stop()`'s last paragraph described how another kernel's sampler waits for writers (counting them and sleeping) and why (its user-stack samples are written by threads). What that tells a reader of this code is the condition this `stop()` relies on: the `sync_exec()` cancel is a barrier only because every writer checks the state and writes with interrupts off. The user-stack target of `.agents/tasks/prof-sampler-object` will write from thread context, so the comment now says what that writer must keep (its check and its write in one interrupts-off section) or what it needs instead (a count of writers that `stop()` sleeps on). That is design decision D8, still open, and the comment no longer pretends either answer.
- The paranoid handler's last paragraph said another kernel gives the NMI its own handler and asserts its general handler never sees one. The fact about this code is the `DEBUG_ASSERT` at the top of `arch_handle_exception()`, so the comment now points at it.

The rest were trailers ("Zircon counterpart: ...") under a paragraph that already said what the function does, or a second reason after the first ("Zircon's answer ... is the kill too"), and were dropped. The NMI comment keeps a general statement in place of the two kernels it cited: the most an NMI handler can leave behind is something lock-free, a counter or a per-CPU buffer printed later from a safe context.

**Change**
Comment text only: every removed or added line of the three source files is a comment line, and the paragraphs the deletions shortened were refilled. Typical rewrites:

```cpp
// Before (faults.cpp, user_exception_type())
/// The mapping is Zircon's (exceptions_c.cc): #UD is its own kind, ...
/// are General -- with one addition: Zircon dies on #CP in every ring,
/// where a ring-3 #CP here is the program's and kills it.
// After
/// #UD is its own kind, ... are General. A ring-3 #CP is among them: it
/// is the program's, so it kills the program rather than the kernel.

// Before (sampler.cpp, stop())
/// Zircon's ThreadSampler::StopLocked() gets the same guarantee differently:
/// it counts writers in flight and sleeps until the count drains, because
/// its user-stack samples are written by threads, not only by interrupts.
// After
/// The barrier holds only because every writer checks the state and writes
/// with interrupts off. A writer in thread context, as the user-stack target
/// will be, keeps that only if its check and its write sit in one
/// interrupts-off section; otherwise it needs another way to be waited for,
/// such as a count of writers in flight that stop() sleeps on until it
/// drains.
```

**Reference**
None, this project's own rule (CLAUDE.md, 2026-10-04). The names the comments carried, for comparison: Zircon `sampler::ThreadSampler`, `ThreadSamplerLock`, `ThreadSampler::StopLocked()` and `ZX_SAMPLER_MIN_PERIOD` (`zircon/kernel/lib/thread_sampler`); Zircon `kernel/arch/x86/exceptions_c.cc`: `try_dispatch_user_exception()`, `arch_dump_exception_context()`, `x86_syscall_process_pending_signals()`, `x86_nmi_handler()`, `x86_exception_handler()`; Linux's NMI-time printk deferral and `noinstr`.

**Verification**
`grep -rn -i -E '(//|/\*|^\s*\*|#|;).*(zircon|fuchsia|linux|\blk\b)'` over `kernel/arch`, `kernel/include/ours/sampler` and `kernel/kernel/sampler` prints nothing. `git diff -U0` of the three source files has no added or removed line that is not a comment. clang-format clean on the changed lines. `boot_check.py --out out/sampler`: build ok, dbg PASS (29 passed, 2 skipped), rel PASS (26 passed, 7 skipped, 2/2 required).

**Deliberately not done**
`kernel/include/ours/task/sched/fair.hpp` (now item 1): the Fair discipline is the user's and no agent edits it.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What property of every sample writer makes a `sync_exec()` of the timer cancel a barrier for `stop()`, and what must a writer running in thread context keep to stay covered by it?
2. Why may nothing on the NMI path take a lock, even one that is only ever taken with interrupts off?
3. Why does `arch_handle_exception()` preempt before it processes signals, and why must the interrupt be fully put away before either?

## Title: Review fixups for the kernel/object sweep: six comments it left imprecise (2026-10-05)

**Files**
- `kernel/object/process_dispatcher.cpp` — finish_dead_transition() counts its four parts, the exception channel among them (commit c9dcdd28)
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp` — State says its explicit values are not load-bearing (commit c9dcdd28)
- `kernel/object/port_dispatcher.cpp` — PortObserver::on_cancel() says maybe_reap() hands the observer back to be freed (commit c9dcdd28)
- `kernel/object/include/ours/object/channel_dispatcher.hpp` — CHANNEL_MAX_PENDING_MESSAGES calls the policy exception D11's planned next step, now possible (commit e26c4e37)
- `docs/repairs/261004/object.md` — item 4 tracks that switch for the user's decision (commit e26c4e37)
- `kernel/object/dispatcher.cpp` — rc_recycle() says where the deferred-deletion list's head lives
- `kernel/object/include/ours/object/process_dispatcher.hpp` — m_handle_table names create_shared(), the declared function that is to share a table
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
None; the comments were wrong or vague, not the code.

**Cause**
Dropping a comparison is not only deleting a clause: the sentence around it was written to lean on the other kernel, and once the name goes, whatever the comparison silently supplied has to be said in ours's terms or it is lost. Six places lost something that way.

- finish_dead_transition() said "Three parts of Zircon's FinishDeadTransition()": three of that function's parts, a count of the other kernel's code. With the name gone, "Three parts." read as a count of this function's, which has four.
- InterruptDispatcher::State said its out-of-order explicit values were "Zircon's numbering". With that gone, nothing told a reader whether `Waiting = 0, Destroyed = 1, ...` mattered; it does not, since nothing reads a state as a number.
- on_cancel() said it worked "through MaybeReap()", and the first rewrite said maybe_reap() frees the observer. It does not: it hands the observer back in a Box, and the Box on_cancel() holds frees it.
- CHANNEL_MAX_PENDING_MESSAGES said a policy exception at the limit "waits for there to be an exception mechanism". The sweep kept that clause, but the mechanism exists now (on_policy_exception(), which on_bad_syscall() uses), so the comment hid a planned step D11 had left open.
- rc_recycle() said "The list lives in Thread::recursive_object_deletion_list()". The thread holds only a pointer; the list's head is a local of the outermost rc_recycle() call, set on the thread while that call runs and cleared before it returns. Where a pointer points is exactly what a reader checks when asking whether it can dangle.
- m_handle_table said it is an Arc "because Zircon's create_shared() gives two processes one table", and the sweep turned that into "as a process created to share another's handles would". ProcessDispatcher declares a create_shared() of its own, not yet defined, so the comment can name it instead of a hypothetical.

**Change**
Comment text only. The two made in this round:

```cpp
// before (dispatcher.cpp, rc_recycle())
/// The list lives in Thread::recursive_object_deletion_list(), and is pushed
/// and popped at its front, so queued objects go last-in first-out.

// after
/// The list's head is a local of the outermost call, and the thread only
/// points at it (Thread::recursive_object_deletion_list()) while that call
/// runs. It is pushed and popped at its front, so queued objects go last-in
/// first-out.
```

```cpp
// before (process_dispatcher.hpp, m_handle_table)
/// Held through an Arc rather than by value so that two processes can
/// share one table, as a process created to share another's handles
/// would; the base class HandleTable already carries the count for it.

// after
/// Held through an Arc rather than by value so that two processes can
/// share one table, as create_shared() is to give them once it is
/// defined; the base class HandleTable already carries the count for it.
```

**Reference**
Zircon `ProcessDispatcher::FinishDeadTransition()`, `InterruptState`, `PortObserver::OnCancel()` with `PortDispatcher::MaybeReap()`, `ChannelDispatcher::WriteSelf()` raising `ZX_EXCP_POLICY_CODE_CHANNEL_FULL_WRITE`, `Thread::recursive_object_deletion_list()` and `ProcessDispatcher::create_shared()`, all already listed in the entry above except `WriteSelf()`.

**Verification**
- Every changed line is a `//` comment line; `scripts/format_cpp.py --check` is clean on the touched files.
- clang-tidy with `-p out/bringup.x86` on dispatcher.cpp and process_dispatcher.cpp: six warnings, all on code lines no fixup touched (missing `override`, missing `[[nodiscard]]`, counter and global name case), so none is new.
- Build and boot, `boot_check.py`: build ok; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required, 0 error lines).
- The sweep grep over `kernel/object`: no hit.

**Deliberately not done**
- ExceptionDispatcher::m_response stays manual-reset, and its comment still says AutoUnsignal was not an option when it was written: switching it is a code change, and with one handle out at a time the two behave the same.
- The switch to a policy exception on a full channel is not made: it changes what a program sees, so it waits for the user (repair item 4).

**How it was found**
Reading each rewritten comment of commit cf27dffb against the code it describes: the parts finish_dead_transition() calls, maybe_reap()'s return value, rc_recycle()'s local `list`, and `git grep create_shared`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- In rc_recycle(), where does the deferred-deletion list's head live, and why is it safe for the thread to point at a stack local?
- Which object frees a PortObserver whose handle is closed, and what happens instead when the port's tear-down got there first?
- Why does a full channel still answer NoResource, and what did D11 plan to replace it with once exceptions existed?

## Title: second review of the kernel/include sweep: two rewritten claims stated something this tree does not do (2026-10-05)

**Files**
- `kernel/include/ours/boot/options.hpp` — `kernel.select.userboot`'s comment says every image here leaves it at the default and what the option exists for, instead of describing images that name a test build of userboot, which this tree does not have
- `kernel/include/ours/task/thread.hpp` — `m_resource_lock` states the rule for the memory allocation policy (it may be replaced only under this lock) instead of describing another thread replacing it, which no code does

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of BootOptions::userboot() was told that an image may name "another userboot build, such as one built to run tests", and could find no such build and no image that sets the option. A reader of `m_resource_lock` was told that "another thread takes this lock to replace" the memory allocation policy, and could find no code that replaces it: `MemoryAllocationState` has `get_policy()` and no setter.

**Cause**
Both are the failure the first review found in ClockSource::Rating, in a milder form. The sweep removed a comparison and kept a fact that belonged to the other system, now worded as if it were true here.

For `kernel.select.userboot` the old text was about another kernel's images: they leave the option at its default, and their eng images name a userboot built to run tests. With the attribution gone, "an image leaves it at the default or names another userboot build, such as one built to run tests" read as a description of this tree's images. Here both images leave it unset (`//kernel/products/image/dev/BUILD.gn` and `dev.dbg_1/BUILD.gn` say so), and no test build of userboot exists. What the sentence has to say in this tree is the rule (the option chooses a build of userboot, never a system service) and the fact (every image takes the default).

For `m_resource_lock` the old text said the lock was modelled on another kernel's per-task allocation lock. The rewrite replaced that with a description of what the lock guards, taken from the comment in `PmNode::Current::apply_thread_policy()`: the policy, "which another thread takes this lock to replace". That is the other kernel's mempolicy path. Here the policy is only read, so the sentence described a writer that does not exist. A reader who goes looking for that writer, to check the lock order, finds nothing. What is true in this tree is the rule a future writer must follow.

**Change**
Comment text only. A comment-stripped comparison of both headers against HEAD shows no code change.

```cpp
// Before
/// picks only *which* one. An image leaves it at the default or names
/// another userboot build, such as one built to run tests; none puts a
/// system service here.
// After
/// picks only *which* one. Every image here leaves it at the default; it
/// exists to name another build of userboot, such as one built to run
/// tests, and never a system service.

// Before
/// Guards what the thread *holds* -- such as its memory allocation
/// policy, which another thread takes this lock to replace -- as
// After
/// Guards what the thread *holds* -- such as its memory allocation
/// policy, which may be replaced only under this lock -- as
```

**Reference**
Zircon's `kernel.select.userboot`, which eng images set to `userboot-test-rust` (`lib/boot-options/options.inc`). Linux `task_lock()` guarding `task->mempolicy` in `do_set_mempolicy()` (`mm/mempolicy.c`).

**Verification**
`scripts/format_cpp.py --check` is clean on both headers. A comment-stripped comparison with HEAD reports no code difference. clang-tidy (`-p out/bringup.x86`) over `kernel/kernel/task/thread.cpp` and `kernel/top/boot_options.cpp`, filtered to the two headers, reports only the existing `readability-identifier-naming` warnings on `SPAWNED_THREADS` and `SPAWNED_THREADS_LOCK`, neither on a changed line. boot_check: build ok; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required).

**Deliberately not done**
"The other design" in `semaphore.hpp` and `clock.hpp` names an alternative that has not been introduced. It is left as it is: each paragraph poses a choice between two options, so the phrase can only mean the rejected one. The comment in `kernel/mm/pm_node.cpp` that the `m_resource_lock` wording came from belongs to the kernel/mm sweep, so it is not touched here.

**How it was found**
A second review of the sweep and of its first review fix. Each rewritten sentence that states a fact about this tree was checked against the code: `git grep 'kernel.select.userboot'` over the GN files found only comments saying the option is left unset, and `git grep 'set_policy\|get_policy'` found a reader and no writer.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Once a comparison with another system is deleted, how do you tell whether a sentence it leaves behind is still true in this tree?
2. Why is "may be replaced only under this lock" correct even though nothing replaces the policy today, when "another thread takes this lock to replace it" is not?
3. What does `kernel.select.userboot` choose, and why is a system service never the answer?

## Title: review fix to the kernel/arch and kernel/phys sweep: the comment over rva() says the macro is unused (2026-10-05)

**Files**
- `kernel/arch/x86/kernel/bootstrap16.S` — the comment over `rva()` says nothing uses the macro, that `__rva_start` is defined nowhere, and how the trampoline really reaches its data
- `docs/changes/261004/comment-rule.md` — this entry

**Symptom**
Latent, cannot be triggered today: no behaviour changes. A reader of `bootstrap16.S` was told that the AP trampoline addresses its own data through `rva()`. Nothing in the file calls `rva()`, and no file in the tree defines `__rva_start`.

**Cause**
The sweep rewrote the sentence that credited `rva()` to another kernel ("we can use this tricky from Linux to have a relative addressing way") into a statement of what the macro does, and the rewrite made a claim about this code: "so the code addresses its own data relative to wherever it was copied". The old sentence said only that the trick could be used; the new one said it is used. Turning a comparison into a fact about ours means checking that fact against ours, which is how the sweep itself caught `xapic.hpp`'s stale "no such variant here yet"; this sentence was not checked.

The trampoline does not need symbol-relative addressing at all. `make_bootstrap_area()` copies the code into the first page of the low buffer and puts `BootstrapData` in the page after it. The real-mode code points `%ds` at that page (`%cs` plus 0x100 paragraphs) and reads `X86_BOOTSTRAP_OFFSET_GDTR` and `X86_BOOTSTRAP_OFFSET_PGD` through it; before the far jump it turns `%ds` into a linear address in `%rbx`, and the long-mode code reads the CPU number, the stack top, the thread and the virtual entry through `%rbx`. The offsets are bare numbers in the assembly because `bootstrap.hpp` pins each one to its member with `static_assert(offsetof(...))`. The one symbol difference the trampoline needs, the offset of its long-mode entry, `make_bootstrap_area()` computes in C++ (`X86_AP_ENTER_LONG_MODE - BOOTSTRAP_AREA_START`).

**Change**
Comment text only.

```text
// Before
/// To sidestep the relocation, rva() names a symbol by its offset from
/// __rva_start, so the code addresses its own data relative to wherever it
/// was copied.

// After
/// rva() names a symbol by its offset from __rva_start, which would let the
/// code address its own data wherever it was copied. Nothing uses it yet, and
/// __rva_start is defined nowhere: the trampoline reaches its data at the
/// fixed X86_BOOTSTRAP_OFFSET_* offsets into BootstrapData, the page after the
/// code, through %ds in real mode and %rbx in long mode.
```

The macro stays. An unused definition may be scaffolding for planned work (`ours-kernel-design`, "Hands off"), and deleting it would not be a comment change.

**Reference**
Linux `rva()` in `arch/x86/boot/compressed/head_64.S` (`#define rva(X) ((X) - startup_32)`), used as `rva(X)(%reg)` with the run-time address of `startup_32` in the register. In this tree `kernel/arch/x86/phys/start64.S` defines and uses its own `rva()` that way, relative to `_start64`.

**Verification**
- Comment-only: every file the sweep and this fix changed, with C and assembly comments (or GN `#` comments) dropped, is identical to `6fa368b7`, the sweep's parent (48 files).
- `clang-format --dry-run -Werror`, limited to the sweep's changed line ranges, is clean on all 35 changed C++ files; `scripts/format_gn.py --check` is clean on the 8 GN files; `scripts/lint_docs.py` is clean on both documents.
- `boot_check.py` after the fix: build ok, `bootstrap16.S` reassembled for both kernels; dbg PASS (29 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (26 passed, 7 skipped, 0 failed, 2/2 required lines, 0 error lines).
- The sweep's greps rerun over `kernel/arch` and `kernel/phys`: only `faults.cpp`'s hits (another session's file, swept on `mvp/integration` by d2cca0c1) and `build_id_note.cpp`'s ELF note convention remain. A list of every `name()` in the two directories' comments that no code in `kernel/`, `system/`, `build/` or `tools/` declares finds only `x86_load_*dt()`, a glob over two real names, and `init_memory()`, below.

**Deliberately not done**
- `kernel/phys/include/ours/phys/legacy_platform_info.hpp` and `kernel/phys/include/ours/phys/init.hpp` name `init_memory()`, which no code defines any more; memory discovery is `MemoryProvisioner::detect()` and `install()` in `kernel/phys/memory.cpp`. It is a stale name from this tree, not a rule-2 hit, and the sweep did not touch it, so it is left for its own fix.
- `docs/repairs/261004/comment-rule.md` item 1 (`faults.cpp`) stays on this branch. `mvp/integration` already swept `faults.cpp` and dropped the item (d2cca0c1), so deleting it here as well would only conflict when this branch's commits are picked across.

**How it was found**
Review of the sweep: each rewritten sentence was checked against the code it describes. `git grep -n '__rva_start\|\brva('` shows the macro's definition and its comment in `bootstrap16.S`, no use there, and no definition of `__rva_start` anywhere.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the AP trampoline read its data at fixed numeric offsets instead of through symbols, and what keeps those numbers in step with `BootstrapData`?
- When a rule-2 rewrite turns "we can use another kernel's trick" into "the code does this", what has to be checked before the new sentence is true?
- Why does `make_bootstrap_area()` compute `X86_AP_ENTER_LONG_MODE - BOOTSTRAP_AREA_START` in C++, and what does that subtraction have in common with `rva()`?
