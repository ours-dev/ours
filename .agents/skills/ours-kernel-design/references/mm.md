# Memory management

## Bring-up order

physboot hands the memory layout over through OBI; a temporary allocator and
mapper take over; the arch/platform layer parses NUMA; `PmNode` registers the
nodes (registration only); `MemMap` builds the PhysAddr-to-frame descriptors
with the temporary allocator; `PmNode` then stands up its own allocators on
`MemMap`. After that: **PMM, CpuLocal, object cache, VMM**. CpuLocal cannot come
after the VMM: `init_vmm()` allocates `ustl::Arc<VmArea>` from the heap, the
heap is the object cache, and `MemCache::init()` needs CpuLocal.

Each `PmZone` sets its frame set up in its constructor, from the zone's pfn
span alone, before the zone is on any list: a zone's free lists are set up
once at boot, never on the allocation path, and every use after that holds the
zone lock (`m_fset` is `TA_GUARDED`). Do not
bring back an attach on first use (2026-10-07): asked outside the zone lock it
races two CPUs meeting a fresh zone, and asked inside it puts the lock ahead of
the per-CPU frame cache, which is meant to run without it. Reference: Zircon's
`PmmNode::Init()`, Linux's `init_currently_empty_zone()`.

## MemMap

- `MemMap` (`ours/mm/memmap.hpp`) is one of two implementations chosen at
  build time, `priv::SparseMemMap` or `priv::VflatMemMap`, via the
  `mm_vflat_memmap` GN argument (`MM_VFLAT_MEMMAP`). Never both, never runtime
  dispatch: `pfn_to_frame()` sits under every physical-memory lookup.
- Both stay classes. Keep the interface as small as Linux's
  `include/asm-generic/memory_model.h` (`pfn_to_page`, `page_to_pfn`,
  `pfn_valid`); bring-up lives in free functions in `ours/mm/memmap_init.hpp`.
  To extend it, list Linux-derived candidates and let the user pick.
- A zone's descriptors are not one array: the sparse model allocates each
  section's on its own. So `FrameBuddy` finds a block's partner from the
  descriptor it is handed, `frame + (buddy_pfn - pfn)`, with the pfn given by
  the zone (`MemMap::frame_to_pfn()`) or a test, and never reaches further than
  one max-order block from it; `SparseMemMap` pins
  `MAX_FRAME_ORDER + PAGE_SHIFT <= SECTION_SHIFT` with a `static_assert`. Never
  index a zone's descriptors off one base (the user's choice, 2026-10-07).
  Reference: Linux `find_buddy_page_pfn()`.

## VmObject

- Offsets in the `VmObject` interface (`read`, `write`, `commit_range` and
  their helpers) are byte offsets typed `VirtAddr`, never page offsets typed
  `PgOff`, because `VmObjectPhysical` need not be page aligned. Resolve any
  mismatch toward `VirtAddr`.
- Never use a bounce page between user memory and a kernel object. Copy
  directly between the VMO page (through the physmap) and the user buffer while
  holding the VMO lock, using the `*_capture_faults` user copy. On a fault:
  drop the lock, `soft_fault()`, retake it, look the page up again, and resume
  from the progress made. Reference: Zircon's
  `VmObjectPaged::ReadUser()`/`WriteUser()`.

## CpuLocal (Linux, not Zircon)

- Addressing follows Linux "model B": `%gs` holds `base(cpu) - origin()`,
  accesses use the variable's own link address (plain `%gs:VAR`), and callers
  hold an `origin + offset` handle, never a naked offset. Model A (`%gs` holds
  the unit base) was tried and rejected. References: Linux
  `arch/x86/kernel/setup_percpu.c`, `arch/x86/include/asm/percpu.h`,
  `mm/percpu.c`, and `.legacy/ours-dev/ours/kernel/main/cpu-local.cpp`.
- The first chunk follows Linux's embed-first layout: physmap by default
  (`pcpu_embed_first_chunk`, units aligned to ride the 2 MB physmap mappings),
  page-table mapping only as the fallback, with a `max_distance` guard so either
  layout can grow.

## Naming

The object allocator is "cache": `MemCache`, `ObjectCache`,
`object_cache.cpp`. "Slab" names only the internal page-level unit.
