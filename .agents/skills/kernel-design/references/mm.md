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
  upper edge of the pressure's bounds. One walk at a time, on `m_reclaim_lock`,
  which `request()` does not take (2026-11-01): OutOfMemory waits out a walk
  already running, then walks itself, because the walk it waited for was asked
  the Warning or Critical distance. That walk is `reclaim_superseding()`, which
  drops the ask the thread has not taken before it walks, so that ask is not
  walked afterwards (2026-11-01; the reclaimer's own entry since 2026-11-17,
  `take()` private). Two asks still waiting keep the larger. The watchdog is
  built holding that reclaimer; `look()` does not call `get()`. A test's `Hold`
  is a `Guard` on the watchdog mutex, dropped before the wake, so lockdep sees
  the hold. `look()` drops `m_lock` for the OutOfMemory walk and reads the
  levels again before it declares (2026-11-02): the walk sleeps, and the mutex
  guards the levels and the declaration. `MemoryPressureLevels::make()` is the
  only builder of a nested value.
- The boot levels are raised so that the delay level sits above
  `zone_floor_frame_count()`, every zone's Low floor for an ordinary
  request capped at its present frames, summed (`covering()`, the user's
  D1(a), 2026-11-14). Then every zone refusing implies free memory below
  the delay level, and the gate, not a zone's Low, decides when an
  ordinary waiting request waits. All three levels move together.
- `MemoryPressureLevels` nests by type (review M10, 2026-11-15): no
  default constructor, `make()` the only builder, and a bad `kernel.oom`
  command line refused there by the watchdog's init hook, which then
  starts no watchdog. The watchdog holds its levels as an `Option`, set
  only by `start()`; tests put back what `levels()` gave them.
- Nothing below the request reads `Gaf` bits (review M4, 2026-11-15):
  `AllocRequest` answers `may_block()`, `may_wait()`, `waits_at_low()`,
  `zeroes()` and `is_confined()`, `flags()` stays the caller's flags
  whole, and `AllocationRun::take()` asks the gate from the request.
- A confined waiting request -- `Gaf::ThisNode`, or a zone range
  narrower than Dma through Normal (`AllocRequest::is_confined()`) -- is
  not put off at a zone's Low: it makes the Min pass and answers `OutOfMem`
  below it (the user's D2(b), 2026-11-14). The machine-wide gate and
  watchdog count memory it cannot use, so nothing would be bound to end its
  wait. A node set narrowed without `ThisNode` is not confined.

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
ProtectionRanges, ZoneList, ZoneCandidates, VmCowPages::Cursor, and
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

A block attempt carries its optional depleted zone in its typed failure. Each
node records its short zones in one atomic word, a bit per zone type
(2026-11-14, N2): the pipeline's `observe_pressure()` marks it and, when the
mark is the node's first since the record was taken, signals the event the
reclaimer registered (`set_reclaim_signal()`, 2026-11-18, review minor item 29);
the thread takes it with `PmNode::take_reclaim_target()`. Neither the node nor
the PMM calls the reclaimer. A mark is made whether or not the reclaimer
listens: one made before it registers is taken when it starts, since it wakes
itself once then, and one made while a Pause lives is kept; the thread takes no
record while a Pause lives (the take asks `paused()` under `m_take_lock`, which
a Pause takes as it begins), a record taken just before is walked rather than
dropped, and the last Pause to end wakes the thread to take what was kept.
Owner-directed block return requires a frame reference at the zone boundary.

## PMM allocation pipeline (user decision, 2026-11-10)

This supersedes the static `PmNode` allocation interface above; the design is
`.agents/tasks/pmm-redesign/DESIGN.md`. It lands one step at a time, so until
a step is in, the code still has the shape described above.

- The `mm::` entries in `pmm.cpp` are the one pipeline across nodes. Each
  call runs on an `AllocationRun` on the entry's stack, private to the file
  (review M3, 2026-11-16): `begin()` resolves the home once, `take(order)`
  makes one block (gate admission, `resolve_request()`, the home node's
  member `alloc_frames(request)`, zeroing and counters), and its destructor
  observes the pressure once, outside the zone lock, however the call ends.
  The release entries observe once at their end. `PmNode` has no static
  allocation or release operation. The static layer was a second entry with
  the same signature that skipped admission, and no production code called
  it.
- The layers under the seam carry the seam's names (the user's, 2026-11-10):
  `PmNode::alloc_frames()` and `free_frames()`, `PmZone::alloc_frames()` and
  `free_frames()`. These replace `take_block()` and `return_block()`; access
  says which layer is the entry, so the name need not. They are sealed with
  passkeys (review items 25 and 40, the user's choice of 2026-11-17,
  `ours/mm/priv/layer_keys.hpp`): the node layer's pair takes a
  `NodeLayerKey` only the pipeline (`priv::Pipeline`, defined in `pmm.cpp`)
  makes, and the zone layer's pair and `adopt()` take a `ZoneLayerKey` only
  `PmNode` and the pipeline make. A friend would open every private member;
  a key opens the calls. Kernel tests have their own door,
  `ours/mm/test/layer_keys.hpp`, which nothing else includes.
  `PmZone::admits()` stays a public query. The watermark setup is
  `PmZone::setup_all_watermarks()`, a static of the zone table beside
  `create_all()` that needs no key; the per-zone steps are private.
- The PMM calls up to no service: the watchdog and the reclaimer both hand it
  an event (`set_free_memory_signal()`, `set_reclaim_signal()`; the second
  since 2026-11-18, review minor item 29, the user's option A after first
  keeping the singleton call). Waiters for a frame are `FrameWaiters`, not the
  gate's: the gate admits and signals the watcher, and wakes one waiter when it
  is armed again.
- The public PMM interface takes primitive parameters, as Linux's
  `alloc_pages()` and Zircon's `pmm_alloc_page()` do: a `Gaf`, an order or a
  count, a node id. Never a compound structure as a parameter; the user
  rejected a `FrameRequest` and a `Placement` aggregate on 2026-11-10. One
  function with a trailing default (`home = ANY_NODE`) replaces the paired
  overloads, no node mask is a public parameter (the pipeline takes one in
  `mm::priv`, `ours/mm/priv/pmm.hpp`, for tests and for a thread's placement
  policy), and whatever else a caller
  wants (zone ceiling, mobility, floor) is a `Gaf` bit behind a preset, not
  a new parameter. `Gaf` keeps `CanBlock`, `CanWait`, `ThisNode` and `Zero`
  and gains `Movable` and `Dma32`.
- Interfaces come in symmetric pairs (`alloc_frames`/`free_frames` at every
  layer, `pin_frame`/`unpin_frame`), and each object
  has one boundary: say what it owns and what it never does before adding a
  method to it.
- `PmNode` is one node: its `ZoneList`, `PageQueues` and CPU masks. The node
  table and the state masks are file-scope data behind free functions in
  `ours/mm/node_table.hpp`, spelled in the flat `node_*` style `pmm.hpp`
  already used (the user's choice, 2026-11-12): `find_node(nid)`,
  `node_mask(NodeState)`, `set_node_state()`, `for_each_node()`,
  `for_each_nid()`. Not `node(nid)`: a local variable named `node` is in
  scope in its own initialiser and would hide it. CPU membership (publish,
  claim, move) belongs to `CpuNodeMap`.
- The scope of the pass was reopened the same day: the user wants the PMM
  designed from what it must support later (swap and eviction, large pages,
  constrained and contiguous allocation, movers), with the node, the zone and
  the frame kept as the principal objects, not a pruning of what exists.
- `pin_frame()` and `unpin_frame()` in `pmm.hpp` were declared and undefined
  when the user said they are needed, for holding a frame against being
  swapped out; they are built now (2026-11-12), over a count in the frame
  header, with the owner's-lock contract written on the declaration.
  `MigrateType` and the `Movable` and `Dynamic` zone types are still unused
  and still planned. An undefined declaration or unused enum in the PMM is a
  statement of a requirement; design for it, never delete it.

## VMM naming choices (user decision, 2026-11-05)

Keep the address selection name alloc_spot because selecting an address may
later include randomization. Retain find_spot in VmaSet, MultiPageRequest,
range_change_locked_object, create_subvma and VmPageSpliceList. Use subvma
for child virtual memory areas.

The approved VMM names are PageSource::request_pages, VmFault::Cause with
the member VmFault::cause, lookup_cursor_locked on the COW and paged VMO
layers, Cursor::PageLookup and VmArea::map_vmo. Use VmArea::UnmapScope with
Mapping and Subarea for the unmap child policy. The placement
helper is resolve_range_locked.

## Zone free-frame count (user decision, 2026-11-05)

PmZone::num_free_frames() reads its atomic free-frame counter without the
zone lock. Buddy transfers update it by the actual frame delta under that
lock. A read can observe the preceding count during an in-flight transfer.
PmZone::count_free_frames() traverses the buddy lists while holding the zone
lock. Document these contracts rather than encoding approximation in the
getter name.

## PMM helper classes (user decision, 2026-11-05)

ZoneCandidates is the filtered range returned by ZoneList::candidates().
CpuNodeMap owns the runtime CPU-to-node table and its per-CPU node-pointer
cache. Allocation origin belongs to AllocRequest as AllocRequest::Origin.
These names replace ZoneSelector, NodePlacement and the namespace-level
Origin without adding a parallel interface.

## PMM threshold and accessor names (user decision, 2026-11-05)

The zone threshold enum is PmZone::Watermark, matching the existing
watermark() and watermark_ok() spelling. Keep its unsigned base and the
Min, Low, High, Count order. Put each variant's explanation above that
variant, with a blank line between entries.

The allocation-state policy accessor is AllocState::policy(), without a
get_ prefix. It still returns a retained Arc; the allocator copies it under
the thread resource lock before using it (`docs/repairs/261116/mm.md` item 1
measures that lock). The policy answers through `AllocPolicy::place(local,
allowed, turn)`, a home node and a node set, and only `resolve_request()`
asks it: for an allocation that names no node and does not carry
`Gaf::ThisNode` (review M7, 2026-11-16). A named node is the caller's choice;
no policy moves it. The kinds follow Linux mempolicy: Preferred moves the
home, Bind narrows the nodes and leaves the home (so the walk stays nearest
first), Interleave moves the home round its nodes by the turn. A policy is
immutable and shared; the turn is the thread's (`AllocState::policy_turn()`),
counted only for a request the policy placed, and reset when the policy is
set. Only the calling thread sets its policy (`mm::set_thread_policy()`). A
request Bind narrowed is confined (`AllocRequest::is_confined()`), so it goes
on to Min rather than wait at Low.
