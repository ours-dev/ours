# flag-consolidation: the commits, in order

**Date**: 2026-10-03
**Status**: Proposed

## Request
Consolidate the kernel's flag enums (DESIGN.md). Scope ends when every surviving flag has a reader and an owner, and the set_flags() overwrite is fixed.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 to D14 | See DESIGN.md | See DESIGN.md | None given | |

## Steps
Markers: [U] waits for a decision in DESIGN.md; [B] changes behaviour. The audit also marked steps that touched the boot-image-pages files; that task has since committed, so the marker is dropped.

Each commit must pass ours-verify (builds, and both images boot). Check `git diff` before staging and commit with `git commit -- <paths>`.

### Frames
1. **Delete page::Flags nothing reads.** Removes Active, Dirty, UpToDate, Foreign, BeginWaiting and Reclaimable, plus PageFrame::mark_active(); init() stops setting Active and UpToDate. No behaviour change. Verify: `git grep` comes back empty.
2. **[B] Fix repair item 1, as in section 4.** Also deletes Pinned, mark_pinned() and its calls (memmap_init.cpp:147 with its comment, init_vmm.cpp:268), and renumbers Reserved=BIT(0), Initialized=BIT(1). Docs and verify as in section 4.
3. **Trim page::Role.** Delete Io, Lru, Heap and Folio. to_string() gets an Mmu case (it prints "Anonymous" today), and the default branch goes so the switch is exhaustive. Document None as Zircon's ALLOC.
4. **[B, debug only] Runtime page tables get Role::Mmu.** Set it in alloc_page_table()'s buddy path; free_page_table() asserts it. Verify: run and exit programs from the shell, so process teardown frees page directories through free_page_table().
5. **[U] Rename Role::Pmm to Role::Free** and drop PmmBody.

### Allocator and caches
6. **Delete MemCache::Option values nobody passes:** Dma, Dma32, Merge, Reclaim and the Alias enumerator, plus the mapping at object_cache.cpp:579-587. Must come before 9.
7. **[U] Delete RedZone, Poison and the CONFIG_MEM_CACHE_DEBUG layout.** If they are kept instead, fix the size added twice at object_cache.cpp:530.
8. **[U] Delete PmZone::Flags, AllocationOptions and gaf_to_allocation_control().** Dropped 2026-10-04: the user kept this scaffolding (D8), so none of the items below is done.
   - Also goes: the options parameter (get_frame_from_zone_list, alloc_frames_direct_reclaim/compact, PmZone::alloc_frames), is_below_high_wmark(), the dead branch at pm_zone.cpp:64-69, and m_watermarks.
   - The watermark and NoFragment TODOs move into a docs/repairs item.
   - The reclaim and compact stubs stay.
9. **Delete the Gaf bits that ALLOWED_GAF strips before use:** Dma, Dma32, Movable, NoFail, NoRetry, Write, DaemonReclaim, Compound and the Reclaim combination.
   - Also goes: gaf_zone_type() and its static_asserts, the zone bound at zone_iterator.hpp:81, NoRetry in GAF_GROW, the never_fail branches and the comment at pm_node.cpp:397, and finally ALLOWED_GAF itself.
   - Update pm_node_test. No behaviour change, because every one of these bits was stripped before use.
10. **Presets become plain constants.** cpu_local.cpp:403 and :447 use `GAF_KERNEL | Gaf::Zero` instead of GAF_BOOT. Delete Gaf::Boot, User and Kernel; define GAF_KERNEL and GAF_USER outside the enum. Fix the comments at cpu_local.cpp:54/402/445, init_vmm.cpp:250 and sparse_memmap.cpp:88.
11. **[U] [B, multi-node only] Delete Gaf::ThisNode** from the presets, GAF_GROW, alloc_frames_preamble, zone_iterator.hpp:83-87 and pm_node_test :280/:291. Verify: also boot a NUMA QEMU configuration if one exists.
12. **[U] [B] Callers ask for Gaf::Zero by name.** GAF_USER no longer carries it, and MemCache::allocate memsets the object when Zero is passed explicitly.
13. **[U] Delete MemCache's creation options.**
    - CacheAlign goes, with a docs/repairs performance item to measure cache-line alignment as an `align` value.
    - Folio goes from its 6 call sites.
    - Then the enum itself, m_option, the Option parameter of create/create_boot/alias/init/compute_layout, the ObjectCache::Flags alias, and AddInitHook's Flags parameter.
14. **[U] Delete ObjectCache::Option::AddInitHook.** ThreadCache and VmaCache call Instance::init(name) from their existing init hooks.

### VMO
15. **Every VMO is lazy unless pinned.** Delete VmoFlags::Lazy, add None, and create() commits only for Pinned (keep the size != 0 skip for now). create_user and the 14 test call sites pass None. [B] for user_copy_test only. Verify with the user_copy, vmo_snapshot and vma_seal tests.
16. **Pinned only where it means something.** VmObjectPhysical::create loses its Flags parameter (vm_area.cpp:286, vmo_dispatcher.cpp:186). reserve() and create_clone() pass None; a slice still inherits, with a comment stating that invariant. m_flags becomes `Flags const`, and the doc is rewritten.
17. **Commit through Zircon's two entry points.** Delete CommitOptions; commit_range(offset, size) remains, and commit_range_pinned() becomes non-virtual and returns InvArgs for length 0. create() calls commit_range_pinned(0, size, true) and drops the size != 0 dodge. Must come after 16.
18. **Delete VmoFlags::Resizable** and reword vm_object_paged.hpp:126.
19. **[U] SupplyOption.**
20. **[U] Rename Pinned to AlwaysPinned.**
21. **[U] [B] create_paged stops pinning.** Also fix userspace-interface.md:115, its "shared VMOs fully committed" text, and K12 in userspace-filesystem.md. Verify: both images boot through init to the shell, and programs run.
22. **Doc only:** justify the VmCowPages::Flags enum by the Flags-word rule and Zircon's VmCowPagesOptions.

### Aspaces and mappings
23. **Delete dead flags.**
    - VmasFlags: OomIgnored, AreaMergeable, Shared, Active, Aslr.
    - VmaFlags: Pinned, Anonymous, Mergeable, Area.
    - VmMapOption::Pinned, at io_apic.cpp:200, local_apic.cpp:776, descriptor.cpp:112, idt.cpp:154, hpet.cpp:347 and stack.cpp:38.
    - Rewrite VmMapOption's doc so that only Sealed outlives the call.
24. **VmaFlags::PermMask = R|W|X.** Delete Share; ROOT_VMAF becomes PermMask | CanMapSpecific; the local RWX at vma_dispatcher.cpp:305 uses PermMask.
25. **Delete VmMapOption plumbing nothing reads:** Builder::set_option and m_option, and create_subvma's option parameter. stack.cpp:38 passes None; fix the stale "Commit" wording.
26. **Rename VmMapOption to VmMapOptions** with a u32 underlying type.
27. **VmAspace takes no creation flags.** Also delete the ArchVmAspace::Flags alias, make both m_kind fields const, and give VmasKind a u8 underlying type.
28. **Rename VmasFlags::Vdso to VdsoClaimed** and renumber: Destroyed bit 0, VdsoClaimed bit 1.
29. **[B, latent] Build a mapping's word from the request.** create_subvma rejects bits outside PermMask | CanMapSpecific. create_mapping builds `allowed | Sealed` (Sealed only when asked). reserve builds `m_vmaf & PermMask`. Active then comes only from activate(); every path calls it.
30. **[B] Check liveness under the lock.** Move the BadState test in VmMapping::protect/unmap and VmArea::protect/unmap after the Guard; today a concurrent destroy() can reset m_vmo between check and use. Extend docs/repairs/261002/mm.md item 3 to the is_active() readers.
31. **[U] VMA lifecycle gets its own field.**
    - A LifeCycleState m_state guarded by the aspace lock: activate() moves it to Alive after the insert succeeds, destroy() moves it to Dead.
    - is_alive_locked() is TA_REQUIRES.
    - VmaFlags::Active and mark_active() go, and m_vmaf becomes const.
    - vma_seal_test:128/133/141 take the lock.
    - This closes 261002/mm.md item 3.
32. **[U] Delete Guest, the create(Kind) overload, clone() and AlsrConfig.**

### Paging and threads
33. **Delete MmuFlags::Present.**
    - Remove it from to_arch/from_arch and the 5 writers: cpu_local.cpp:51, physboot.cpp:146, elf_image_mapping.cpp:112, kernel/phys/aspace.cpp:96, kernel/arch/x86/phys/aspace.cpp:43.
    - Update the ProtectVisitor comment, paging_test and x86_paging_smoke.
34. **VmMapping::map and its helpers lose the ignored `commit` bool.**
35. **Delete MapOptions.**
    - The parameter goes from every layer and from MappingCoalescer, along with the re-exports.
    - Document that an existing entry is always an error.
    - Rewrite docs/repairs/260903/mm.md:70-74: Zircon's ExistingEntryAction comes back on map_bulk only if a caller needs it, and Upgrade-instead-of-remap on the fault path is a performance item.
36. **Delete UnmapOptions** and its parameter on all four layers.
37. **[U] Delete HarvestOptions and the harvest_accessed declaration.** Then controls.hpp, its entry at kernel/lib/arch/BUILD.gn:16 and its includes go.
38. **Delete ThreadDispatcher::Flags**, m_flags, mark_active() and the call at process_dispatcher.cpp:217.
39. **[U] [B] Drop Discache from the bootstrap16 identity maps** (bootstrap.cpp:107, :111) and document Discache as PCD-only. Verify: all APs come online on both images.

### Naming
40. **Rename bootmem::MemoryType::Mmu to Managed.**

### Repair items to record (no code)
- The page::Status word written under mixed protection.
- User faults never check MmuFlags::User (Zircon does, vm_mapping.cc:1201).
- protect_locked replaces the stored record, so it can drop User and Discache; fix before vma_protect (P1-F).
- Upgrade-instead-of-remap on the fault path (performance).
- CacheAlign alignment (performance).
- Watermarks.
- Freeing a boot-built page table.

## Files
See each step; DESIGN.md's per-flag table names the files behind each flag.

## Test plan
As DESIGN.md's testing section, plus each step's own check.
