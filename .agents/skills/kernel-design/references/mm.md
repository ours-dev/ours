# Memory management

## Strong address types (user decision, 2026-11-03)

The user explicitly prefers distinct strong VirtAddr and PhysAddr types in
`system/public/ours/types.hpp`, in the enum-class form they drafted, because
they need address type safety. Preserve that intent. Do not replace them with
primitive aliases merely to make an unrelated integration compile. The user
subsequently requested removing VmPlacement and completing both address
migrations in this task (2026-11-03). Address plus/minus a byte offset
preserves the domain; subtracting two addresses in one domain yields a byte
offset. Addresses do not implicitly convert to integers or each other, and two
addresses cannot be added. Machine encodings and ABI records convert
explicitly. VirtAddr reserves the top sixteen numeric values for special
requests: Invalid is all ones and Anywhere is one below it. Zero remains a
concrete address. The kernel virtual layout excludes the page containing these
values.

VmArea creation interfaces now take VirtAddr directly: Anywhere requests a
free spot, while zero and other concrete addresses request fixed placement.
Invalid and unassigned reserved values fail with InvArgs before alignment.
Fixed placement still requires CanMapSpecific and refuses overlaps with
AlreadyExists. VmAspace creation rejects any range intersecting the reserved
values, so automatic placement cannot return one.

Address constants use the global `_va` and `_pa` literal suffixes in
`system/public/ours/types.hpp` (user request, 2026-11-03). Like the integer
suffixes in prelude.hpp, they are available without a namespace import and
produce the corresponding scoped address type in constant expressions.

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
  their helpers) are byte offsets typed `usize`, never virtual addresses or
  page offsets: an object has no virtual address, and `VmObjectPhysical` need
  not be page aligned. This supersedes the earlier `VirtAddr` spelling now
  that VirtAddr is a distinct strong type (2026-11-03).
- Never use a bounce page between user memory and a kernel object. Copy
  directly between the VMO page (through the physmap) and the user buffer while
  holding the VMO lock, using the `*_capture_faults` user copy. On a fault:
  drop the lock, `soft_fault()`, retake it, look the page up again, and resume
  from the progress made. Reference: Zircon's
  `VmObjectPaged::ReadUser()`/`WriteUser()`.

## COW range notifications (user decision, 2026-11-04)

A COW node decides the range and timing of page-table invalidation and
synchronously notifies one representative VMO under the tree lock. The
representative holds a flat list of other views over the same node. Each
view clips the original range against its own window and updates its
mappings. A slice joins its backing node's view group regardless of its
creation parent. The representative hands off the list under the tree lock
before destruction. Page movement or release follows the notification.

## Private VMM flags (user decision, 2026-11-04)

A private flags word can include mutually exclusive bits when the owning
class controls every transition. Document legal combinations and the
transition that enforces them on the enum. Expose named operations, and
update the word under one synchronization discipline. PageSource replaces
Detached with Closed under its source lock. Both stop new page requests.

## Mapping options (user decision, 2026-11-04)

The VMM uses one scoped MapOptions bitmask. ModeMask selects Lazy when
zero, Resident for existing pages, or Commit for owned pages. Creation
rejects conflicting modes and unknown bits before registration. Sealed
combines with any one mode. Explicit installation accepts only Resident
or Commit. The user ABI translates MapRange to Resident.

Use Sealed for mappings whose address and permissions must survive ordinary
unmap and protect requests. Use VMO pinning to express physical residency.
The kernel still tears down sealed mappings during address-space destruction.

## Page aging (decided 2026-10-25, built 2026-10-30)

- Recent use of a page is recorded in two ways. The hardware sets the A bit
  of the page-table entry an access went through, and the scanner harvests
  it. A software mark records what no such entry sees: the kernel reading or
  writing the page through the physmap (`vmo_read`, `vmo_write`,
  `process_read_memory`). The scanner never harvests physmap entries: they
  are large leaves, every kernel access sets them, and clearing a global
  entry needs a shootdown.
- The software mark is made where `VmCowPages::Cursor` hands a page out
  (`require_page()`, `require_owned_page()`, `maybe_page()`), on by default.
  A hardware fault (`VmfCause` without `Software`) and a populate turn it
  off, since the access that follows goes through the entry and sets its A
  bit. A soft fault marks too, though its retry goes through the program's
  entry: twice is harmless, and the rule stays one bit.
- Default on because a forgotten opt-out only marks twice, while a forgotten
  mark gets a page in use reclaimed. The user's choice, 2026-10-25, over a
  mark by each caller after it reads (Linux `folio_mark_accessed()` in
  `filemap_read()`), which fails the unsafe way, and over a page handle whose
  `access()` is the only way to the bytes and marks, which is exact but
  changes how every caller reaches a page. Reference: Zircon
  `VmObjectPaged::GetPage()`, which calls `DisableMarkAccessed()` on its
  cursor for `VMM_PF_FLAG_HW_FAULT`.
- Built: one aging thread per node rotates the ring every
  `kernel.mm.page-aging-seconds` (5 by default); every exit of the cursor
  goes through `handed_out()`, and `disable_mark_accessed()` is the opt-out.
  `PageQueues::kind_of()` and `mark_accessed()` read the queue byte under the
  queue lock: the aging thread rewrites it under that lock alone, so a VMO's
  lock does not make an unlocked read safe.
- The harvest runs in the aging thread before each rotation: for every user
  address space, every mapping of a paged object, the page table clears the
  accessed bit of each leaf the node's queues take (`take_accessed()`), and
  one whole-space shootdown follows (`ArchVmAspace::invalidate_all()`, a CR3
  reload on each CPU running the space; without it a hot page hits the TLB
  and never sets the bit again). The visitor runs under the page table's spin
  lock and takes the queue lock inside it: VMO lock, page-table lock, queue
  lock. Each node's thread keeps its own pages' bits and leaves the others'.

## Page reclaim (decided 2026-10-30, pager pages built)

- A reclaimer finds a page on a queue with only a raw `VmCowPages *`. It makes
  it a reference with `Rc::try_adopt()` under the queue lock, which is safe
  because the owner's destructor takes its pages off the queues under that lock
  before its storage goes. It then takes the owner's lock and checks the page
  again before it does anything (`VmCowPages::reclaim_page()`), and drops the
  reference with no lock held.
- Reclaimable means clean. A page of a node with a page source is mapped
  read-only (`Cursor::writable_here()`), so the first write comes back to
  `require_owned_page()`, which files the page anonymous
  (`file_dirty_locked()`). Until a pager VMO can be written back, such a page is
  never evicted.
- A pin counts as a write (2026-11-03): `pin_range_locked()` takes each page
  through `require_owned_page()`, since a device may write it, and the queue is
  the only record of "written". The pinned queue holds the page meanwhile, so
  the last unpin files a page of a node with a page source as written
  (`Fill::Written`), never as the source's content. A page pinned once is not
  reclaimable again; a read-only pin that kept it clean needs a per-page bit
  (`docs/repairs/261030/mm-pin.md` item 4).

- Zero pages are deduplicated in leaf nodes only
  (`VmCowPages::dedup_zero_page()`):
  look, take Write from every mapping, look again, unmap, swap the page for a
  marker in place. The second look is the one that counts, since a write then
  faults and needs the lock. A hidden node's pages are shared with the nodes
  below, whose mappings the hidden node cannot reach, so they stay.

- `MemoryReclaimer` runs only when the watchdog asks (D3 of page-reclaim): a
  request at Warning and Critical, served on its own thread, and a synchronous
  `reclaim()` on the watchdog's thread at OutOfMemory, before anything is
  refused. Zero pages go before pager pages. The target is the distance to the
  upper edge of the pressure's bounds. One walk at a time, on
  `m_reclaim_lock`, which `request()` does not take (2026-11-01): OutOfMemory
  waits out a walk already running, then walks itself, because the walk it
  waited for was asked the Warning or Critical distance. That walk calls
  `take()` first and drops the result, so an ask the thread has not taken
  is not walked afterwards (2026-11-01). Two asks still waiting keep the
  larger. The watchdog is built holding that reclaimer; `look()` does not
  call `get()`. A test's
  `Hold` is a `Guard` on the watchdog mutex, dropped before the wake, so
  lockdep sees the hold. `look()` drops `m_lock` for the OutOfMemory walk
  and reads the levels again before it declares (2026-11-02): the walk
  sleeps, and the mutex guards the levels and the declaration.
  `MemoryPressureLevels::make()` is the only builder of a nested value.

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

The user clarified on 2026-11-04 that Pm/Vm prefixes mark principal memory
models and are not mandatory on every helper. Prefer the repository's existing
concrete helper names and containing-object context: PageQueues,
ProtectionRanges, ZoneList, ZoneSelector, VmCowPages::Cursor, and
VmObject::Builder. Consolidate terminology rather than mechanically adding
prefixes or role suffixes. Specific PMM helper renames remain design
proposals, not accepted implementation decisions.

## PmNode allocation interface (user decision, 2026-11-04)

PmFrame, PmZone, and PmNode are the principal PMM resource models. The user
selected PmNode::alloc_frames and PmNode::free_frames as static operations
across nodes. Node instances retain their resource state; the static allocation
interface selects a home node and walks permitted fallback zones. Retain the
implicit-local and explicit-home calling forms and existing restrictions.
This supersedes the separate FrameAllocator class in the earlier proposal.

`Gaf::ThisNode` follows Linux's `__GFP_THISNODE` placement contract.
A request may use only its explicit home node or, for implicit allocation,
the current CPU's node. The resolver intersects the caller's allowed mask with
that node, so an excluded or exhausted home yields allocation failure. The
implicit path skips thread placement policy. References include Linux
`gfp_zonelist()`, `prepare_alloc_pages()` and `alloc_frozen_pages_noprof()`.

`ZoneList::candidates()` filters the stored zone order against a resolved
request. `PmZone::take_block()` and `return_block()` own block transfer.
`PmNode` completes zeroing outside the zone lock and compares the actual
supplier with the captured initiator node for locality. Its static return
path validates frames for both single and list release.

`mm::alloc_frames()` is the result-returning interface for immediate block
allocation and `Gaf::CanWait` pressure admission. `ShouldWait` lets the caller
release its locks before waiting. `wait_for_frame_allocation()` serves the
default-placement single-page wait. The static `PmNode` operations perform
the physical transfer.

Buddy transfers and their free-count deltas share the zone lock. Public
static node allocation and return operations observe pressure once after
completion, with staged blocks restored before observation on error.

A failed order-zero walk may report an eligible zone below High. The
background reclaimer resamples queued zones and walks the selected node's
page queues. Admission and waits use the global gate. The node-scoped
reclaim request is advisory and preserves the immediate allocation result.

A block attempt carries its optional depleted zone in its typed failure.
The reclaimer stores Idle and Pending as per-zone enum states. Owner-directed
block return requires a frame reference at the zone boundary.
