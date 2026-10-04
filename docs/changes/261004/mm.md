# mm changes

## Title: Frame flag setters stop erasing each other, and Reserved stays a flag beside the role (flag-consolidation step 2) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `set_flags()` and `mark_pinned()` go, along with `Flags::Pinned`; `mark_reserved()` ORs its bit in and asserts it was clear; Reserved and Initialized are renumbered to bits 0 and 1, which shrinks the Flags field of the Status word from 10 bits to 2; Role and Flags document which is a use and which an attribute; `become()`'s doc no longer claims to be the role's only writer
- `kernel/mm/memmap_init.cpp` — `stamp_reserved_frames()` skips a frame already Reserved, out loud; page tables are no longer marked Pinned
- `kernel/mm/init_vmm.cpp` — the zero page is no longer marked Pinned
- `kernel/mm/include/ours/mm/pmm.hpp` — `unwire_frame()`'s doc no longer leans on the bug
- `docs/repairs/261004/x86-mm.md` — new: physboot's identity-map page tables are unhooked and never freed, found by this change's review
- `docs/repairs/261003/mm.md` — deleted: item 1 was its only item
- `.agents/tasks/flag-consolidation/DESIGN.md` — D1 decided

**Symptom**
Latent. Measured on both images before the change: all 12 boot page-table frames ended with Role::Mmu and only `Pinned` set, without `Reserved`. Every reserved frame ended without `Initialized`, and the zero page lost `Initialized` too. The guard that no Reserved frame reaches the buddy (`free_frames_phys_range()`) could not see a page table.

**Cause**
The objects:
- A frame descriptor's Flags field: three flags (Pinned = bit 1, Reserved = bit 8, Initialized = bit 9) in a 10-bit field of the Status word.
- `set_flags()`: `m_status.set<Index::Flags>(flags)`, which writes the whole field. Every `mark_*()` went through it.
- `stamp_reserved_frames()`: for each reservation's frames, skip a descriptor without Initialized, mark it Reserved, give a page table Role::Mmu, and add one to the node's and zone's reserved count.

The events at boot:
1. `init()` sets Initialized on every usable frame. A hole gets `init()` and then `mark_reserved()`, so its flags become Reserved alone: Initialized is gone.
2. The stamp marks a frame Reserved, which also erases Initialized. A page table is then marked Pinned, which erases Reserved.
3. If a second reservation covered the same frame, or one covered a hole, the stamp would find no Initialized and skip it.

So the overwrite was doing two jobs nobody had written down. It lost Reserved on page tables, which was the bug. It also de-duplicated the stamp: a frame could not be counted twice, because the first mark made the second visit skip it. The repair item proposed an OR and said the counts "may grow, which is the fix showing". That was wrong: an OR alone would count holes and shared frames, which is a new bug. The counts must stay the same, and the de-duplication must become a stated test.

The temporary counters (2026-10-03) found 636 hole frames on each image. No reservation covered a hole, and none shared a frame with another. So the de-duplication never fired on the QEMU images. It is still needed, because a firmware memory map can produce either case.

**Change**
Before:

```cpp
constexpr auto set_flags(Flags flags) -> void { m_status.set<Index::Flags>(flags); }
constexpr auto mark_pinned() -> void { set_flags(Flags::Pinned); }
constexpr auto mark_reserved() -> void { set_flags(Flags::Reserved); }
...
frame->mark_reserved();
if (role != page::Role::None) {
    frame->set_role(role);
    frame->mark_pinned();
}
```

After:

```cpp
auto mark_reserved() -> void {
    DEBUG_ASSERT(!(flags() & Flags::Reserved), "frame reserved twice");
    m_status.set<Index::Flags>(flags() | Flags::Reserved);
}
...
if (!!(frame->flags() & page::Flags::Reserved)) {
    continue; // shared with an earlier reservation, or a hole
}
frame->mark_reserved();
if (role != page::Role::None) {
    frame->set_role(role);
}
```

`init()` is now the only whole-field write. Pinned goes rather than being fixed: nothing read it, and in Zircon a pin is a count on the page, not a frame flag. Its two writers were page tables and the zero page. Page tables are covered by Role::Mmu. The zero page is covered by nothing, and needs nothing today: nothing here reclaims, migrates or frees a VMM page on its own. Zircon marks its zero page WIRED (`zircon/kernel/vm/vm.cc`, `vm_init_preheap()`); the equivalent here would be Reserved, not done, because Reserved also means being on the boot-time reserved books.

Decision D1 of `.agents/tasks/flag-consolidation/DESIGN.md` chose between this and making Reserved a role, `Role::Wired`, as in Zircon's one-state-per-page model. The user chose the flag: Role is what a frame is used for, and "held since boot" is not a use but a fact about a frame whatever its use. So a boot page table is Mmu and Reserved at once, and the kernel image is None and Reserved. The Role and Flags docs now say so.

**Reference**
- Linux keeps the same split: `page_type` is the use, and `PG_reserved` is a separate bit set with `SetPageReserved()`, a `set_bit()` (`include/linux/page-flags.h`). `free_pagetable()` in `arch/x86/mm/init_64.c` tests `PageReserved` on a table.
- Zircon instead gives a page one `vm_page_state`. Physboot's page tables become `MMU` at hand-off (`X86ArchVmAspace::HandoffPageTablesFromPhysboot()`), and the other reservations become `WIRED` (`PmmNode::InitReservedRange()`). That is option B, not taken.

**Verification**
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed).
- The six per-type `Reserved N frames (K KiB) of <type>` lines are identical to the baseline taken at `38808623`, on both images.
- In a scratch copy:
  - all 12 page-table frames are Mmu and Reserved;
  - a second call to `stamp_reserved_frames()` logs no counts, so every frame was skipped;
  - with the skip removed and the stamp run twice, dbg stops at "Assertion failed on !(flags() & Flags::Reserved) with message: frame reserved twice", and rel, with no assertions, logs every count twice.

**Deliberately not done**
- `Role::Wired` (option B), for the reason above.
- Retiring Initialized. It is the only way to tell a descriptor nothing initialised (Role::None is zero), as the audit found.
- The page::Status word race (the buddy writes Role and Order under the zone lock, owners write Role and Flags with none). Recorded as an open question in the DESIGN, the same under either option.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`) found that the repair item's own Change and Acceptance were wrong. Temporary counters in `stamp_reserved_frames()`, which classified each visited frame (first visit, hole, visited before, page table visited before, never initialised), measured how often each case occurs. A second check of the D1 arguments against the Linux and Zircon sources found that the premises first given for A did not hold: the managed count is a free count, nothing frees a boot page table, and Zircon does count per state. A now rests on the user's reason instead.

**The user's prediction**
"A", then the reason: Role is a frame's actual use, not an attribute of a kind of page. The analysis first given for A rested on three premises that a second check refuted. The user's reason is different and holds: it is the line Linux draws, and `role_of()` in `memmap_init.cpp` already described None-and-Reserved frames that way. A's one concrete advantage also matches the hint given: `become()` rewrites only the role, so a Reserved flag survives a missed `unwire_frame()`, and the free path's assert catches it.

**Three questions you should be able to answer**
- Which two jobs did the whole-field `set_flags()` do, and why did fixing it with a bare OR need a third change?
- Why is a boot page table both Mmu and Reserved, and what would option B have made of it?
- Why does `mark_reserved()` assert the bit was clear, and what did the scratch run without the skip show in dbg and in rel?

## Title: page::Role keeps only the roles something writes, and its to_string() is exhaustive (flag-consolidation step 3) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — Io, Lru, Heap and Folio go; the enum's doc says why, and None is documented as Zircon's ALLOC; the Role field of the Status word shrinks from 4 bits to 3
- `kernel/mm/pm_frame.cpp` — `to_string(Role)` loses the three dead cases and its default branch, and gains an Mmu case

**Symptom**
`to_string(Role::Mmu)` printed "Anonymous", so a dumped boot page table looked like a frame with no role at all. Nothing else is observable: no code ever set the four removed roles.

**Cause**
The objects:
- `page::Role`: which payload of a frame descriptor is live, one value at a time.
- `to_string(Role)`: a switch with a `default:` that returned "Anonymous".

The four roles were names, not uses. Io had no stated meaning. Lru and Folio are Linux ideas that are not exclusive uses: in Linux, LRU membership is the PG_lru flag and a compound head is PG_head, both beside the page's type. Here, LRU membership is the page queue a Vmm page sits on (`VmmBody::queue`), and compound pages are the `m_compound` stamp, which a Slab of order above 0 carries at the same time as Role::Slab. Heap was the slot the vmalloc design considered and rejected: kmalloc pages are Slab and vmalloc pages are Vmm.

The `default:` branch is why Mmu printed wrong. When Mmu was added, the compiler could not say the switch missed it, because a default makes every switch complete. Without a default, `-Wswitch` names every unhandled enumerator, so the next role added cannot be forgotten here.

**Change**
Before:

```cpp
enum class Role { None, Io, Lru, Vmm, Pmm, Slab, Heap, Folio, Mmu, Count };
...
        case PageFrame::Role::Heap:
            return "Heap";

        default:
            return "Anonymous";
    }
}
```

After:

```cpp
enum class Role { None, Vmm, Pmm, Slab, Mmu, Count };
...
        case PageFrame::Role::Mmu:
            return "MMU";
        case PageFrame::Role::Count:
            break;
    }
    return "Invalid";
}
```

The `Count` case and the return after the switch follow `bootmem::to_string(MemoryType)`. The values move (Vmm 3 to 1 and so on); nothing stores a role outside the Status word, so nothing depends on them.

**Reference**
- Zircon `vm_page_state` (`zircon/kernel/vm/include/vm/page.h`): FREE, ALLOC, OBJECT, WIRED, HEAP, MMU, SLAB, IOMMU, IPC, CACHE. ALLOC is our None, OBJECT our Vmm, SLAB our Slab, MMU our Mmu. Zircon's HEAP is cmpctmalloc's pages, which this kernel does not have.
- Linux `page_type` (`include/linux/page-flags.h`, PGTY_buddy, PGTY_slab, PGTY_table and so on) is exclusive; PG_lru and PG_head are flags beside it.

**Verification**
- `boot_check.py --out out/flag-consolidation`: dbg PASS (24 passed), rel PASS (21 passed).
- clang-tidy on `pm_frame.cpp`: nothing on the changed lines. clang-format: clean.
- `git grep` for `Role::Io`, `Role::Lru`, `Role::Heap` and `Role::Folio` under `kernel/` is empty.

**Deliberately not done**
- Renaming Pmm to Free, Zircon's name for the same state. It is decision D11, step 5.
- Giving runtime page tables Role::Mmu. It is step 4.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the page::Role rows): `git grep` found no writer for the four roles, and found that `to_string()` had no Mmu case.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a frame not have a "Folio" role here, when it can have a Slab role?
- What did the `default:` branch cost when Mmu was added, and what replaces it?
- Which Zircon page state is our Role::None, and which frames carry it?

## Title: Page tables the buddy gives out are Role::Mmu, and free_page_table() asserts it (flag-consolidation step 4) (2026-10-04)

**Files**
- `kernel/mm/pmm.cpp` — `alloc_page_table()`'s buddy path sets Role::Mmu on the frame; `free_page_table()` asserts the frame is still Mmu before handing it back
- `kernel/mm/include/ours/mm/pmm.hpp` — `free_page_table()`'s doc states the precondition
- `kernel/mm/include/ours/mm/pm_frame.hpp` — Role::Mmu's doc names its writers and its check, and cites the x86 Zircon function instead of the ARM one

**Symptom**
Latent. A page table allocated after the buddy came up was Role::None, the same as a frame with no owner, so only the 12 tables physboot built carried Mmu. `free_page_table()` took any frame: a caller handing it a VMO page or a slab would have freed it to the buddy without complaint.

**Cause**
The objects:
- Role::Mmu: the role that says "this frame is a hardware page table".
- `alloc_page_table()`: before the buddy, it takes a page from EarlyMem with `MemoryType::PageTable`; after, it calls `alloc_frames()`, which returns a frame with Role::None.
- `stamp_reserved_frames()`: at the handover, it gives every `MemoryType::PageTable` reservation Role::Mmu.
- `free_page_table()`: `PageTable::destroy()` calls it for every table of a user address space when the address space dies.

So the role was set only on one side of the handover. The tables built before it went through the stamp; the ones built after it, every user address space's tables, never passed anything that would set a role. Role::Mmu's own doc cited Zircon marking every page-table allocation MMU, which the code did not do. A role that only some page tables carry cannot be checked, so the free path checked nothing.

**Change**
Before:

```cpp
auto *frame = alloc_frames(node, GAF_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE));
return frame ? frame_to_phys(frame) : PhysAddr{0};
...
free_frames(phys_to_frame(base), get_order(size / PAGE_SIZE));
```

After:

```cpp
auto *frame = alloc_frames(node, GAF_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE));
if (!frame) {
    return PhysAddr{0};
}
frame->set_role(page::Role::Mmu);
return frame_to_phys(frame);
...
auto *const frame = phys_to_frame(base);
DEBUG_ASSERT(frame->is_role(page::Role::Mmu), "freeing {:#x} as a page table, but its role is {}", ...);
free_frames(frame, get_order(size / PAGE_SIZE));
```

The frame goes back to the buddy still Mmu, not reset to None first. That is the rule `object_cache.cpp`'s slab destroy() states: only the buddy's own `insert_frame()`, under the zone lock, may write Role::Pmm, because `FrameBuddy::is_mergeable()` reads Pmm as "on my free list". Every caller of `free_frames()` hands frames back in the role it used them in.

The role is set on the head frame only. x86 tables are one page, so the order is always 0.

A boot-built table freed through `free_page_table()` would pass the new assert, since it is Mmu, and then trip the Reserved assert in `PmNode::free_frames()`. Nothing frees one today; the kernel address space is never destroyed.

**Reference**
- Zircon `X86PageTableImpl::AllocatePageTable()` (`kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`) calls `set_state(vm_page_state::MMU)` on every table page it allocates, and `ConsistencyManager::queue_free()` in the same file asserts `state() == vm_page_state::MMU` before a table goes back to the PMM.
- Linux marks page-table pages `PGTY_table` in `pagetable_alloc()`/`__pagetable_ctor()` and clears it in `pagetable_dtor()` (from memory, not checked out).

**Verification**
- `boot_check.py --out out/flag-consolidation`: dbg PASS (24 passed), rel PASS (21 passed).
- The dbg log shows 30 address spaces destroyed ("mm: aspace '...' destroyed": userboot, `bin/fault_probe` runs and the shell's other programs). Each one frees at least its root through `free_page_table()`, so the assert ran on every one and never fired.
- clang-tidy on `pmm.cpp`: nothing on the changed lines (two pre-existing naming warnings in `pmm.hpp`). clang-format: clean.

**Deliberately not done**
- Making the EarlyMem path set the role as well. Those tables are stamped Mmu at the handover, and EarlyMem has no frame descriptors to write before then.
- Freeing boot-built tables. That needs an un-wire path that accepts Role::Mmu, which is `docs/repairs/261004/x86-mm.md` item 1.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the page::Role Mmu row) compared Role::Mmu's doc with its writers and found only the stamp.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Before this change, which page tables were Role::Mmu and which were not, and why did it depend on when they were allocated?
- Why does `free_page_table()` hand the frame back still Mmu instead of resetting it to None?
- What happens in a debug build if something frees a page table physboot built, and which assert stops it?

## Title: PmmBody goes, so only the buddy can say a frame is free; Role::Pmm keeps its name (flag-consolidation step 5) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::PmmBody` and its two static_asserts go; Role::Pmm gets a doc comment naming its one writer, its one reader and Zircon's FREE; the two comments that pointed at PmmBody now point at the comment above VmmBody
- `kernel/mm/object_cache.cpp` — the history in slab `destroy()`'s comment no longer names a type that exists
- `.agents/tasks/flag-consolidation/DESIGN.md` — D11 decided

**Symptom**
Latent. `PmmBody` was an empty struct that nothing used. Its only effect was to make `frame->become<PmmBody>()` compile, and that call writes Role::Pmm from anywhere, without the zone lock.

**Cause**
The objects:
- Role::Pmm: "this frame is on a buddy free list". `FrameBuddy::insert_frame()` writes it under the zone lock; `remove_frame()` writes None.
- `FrameBuddy::is_mergeable()`: on a release, it reads the neighbour's role, and Pmm means "free, coalesce with it".
- `become<Body>()`: zeroes the payload and writes `Body::ROLE` into the Status word.

The events, as they happened twice before:
1. Something other than `insert_frame()` writes Pmm on a frame that is not on a free list: slab `destroy()` through `become<PmmBody>()`, before `free_frames()` reached the lock; or `frame_to_phys()`, through a cast helper, on a live frame.
2. Another CPU releases that frame's buddy, reads Pmm, and coalesces.
3. The slab case unlinks a node that was never linked, and the intrusive list traps (#UD in `FrameBuddy::release_inner()`). The `frame_to_phys()` case puts live memory into a free block, which is handed out twice.

Both fixes stopped the writer but left the door: a body type for Pmm is an invitation to write the role through `become()`. Pmm has no payload, so the type carried nothing else.

**Change**
Before:

```cpp
struct PmmBody {
    constexpr static auto ROLE = Role::Pmm;
};
...
static_assert(sizeof(page::PmmBody) <= PageFrame::BODY_SIZE);
static_assert(std::is_trivially_copyable_v<page::PmmBody> &&
              std::is_trivially_destructible_v<page::PmmBody>);
```

After: the struct and both asserts are gone, and the role says why:

```cpp
/// On a buddy free list. Only FrameBuddy::insert_frame() writes it,
/// under the zone lock, and FrameBuddy::is_mergeable() reads it on a
/// neighbour as "free, so coalesce with it". ...
/// That is why it has no body type: become() would let any caller
/// write it. Zircon's FREE, Linux's PageBuddy.
Pmm,
```

Decision D11 asked whether to rename Pmm to Free, Zircon's name for the state, because "Pmm" also reads as true of an allocated frame with no owner (Role::None). The user chose to keep the name; the doc comment carries the meaning instead.

**Reference**
- Zircon `vm_page_state::FREE` (`zircon/kernel/vm/include/vm/page.h`): a page on the PMM's free list, set by the PMM node when it takes a page back.
- Linux `__SetPageBuddy()` in `set_buddy_order()` (`mm/page_alloc.c`, from memory): only the buddy allocator, under `zone->lock`, marks a page PageBuddy, and `page_is_buddy()` tests it when merging.

**Verification**
- `git grep PmmBody` under `kernel/` finds only the history in `object_cache.cpp`.
- `boot_check.py`, in a scratch worktree at `8058d51d` with this change applied (the shared tree held another session's half-done exception-channel edit that did not compile): dbg PASS (24 passed), rel PASS (21 passed).
- clang-tidy on `pm_frame.cpp` (which includes the header): nothing reported. clang-format: clean.

**Deliberately not done**
- Renaming Pmm to Free or Buddy: D11, the user kept the name.
- Making `set_role(Role::Pmm)` refuse callers other than the buddy. `set_role()` is public, so a determined caller can still write Pmm; the type is gone, which removes the easy path, not every path.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the page::Role Pmm row) found that `become<PmmBody>()` had no caller.

**The user's prediction**
None: a rename decision, not a prediction.

**Three questions you should be able to answer**
- Which function is the only one that may write Role::Pmm, and under which lock?
- What goes wrong when a frame says Pmm while it is on no free list, and what did each of the two past cases look like?
- Why was deleting an unused empty struct a safety change rather than a tidy-up?

## Title: MemCache::Option loses the five values no caller passes, and their mapping onto Gaf (flag-consolidation step 6) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/object_cache.hpp` — Dma, Dma32, Merge, Reclaim and Alias go; the enum's doc says why
- `kernel/mm/object_cache.cpp` — `MemCache::init()` no longer turns Dma, Dma32 and Reclaim into Gaf bits; `alias()`'s TODO no longer names the deleted value

**Symptom**
None observable: no cache was ever created with these options.

**Cause**
The objects:
- `MemCache::Option`: creation options for a slab cache, copied by name from Linux's SLAB_* flags.
- `MemCache::m_gaf`: the Gaf every slab refill of that cache allocates its frames with.
- `MemCache::init()`: turned Dma into `Gaf::Dma`, Dma32 into `Gaf::Dma32` and Reclaim into `Gaf::Reclaim` on `m_gaf`.

Each value stood for a Linux feature this kernel does not have, and two were wrong even as copies:
- Dma and Dma32 asked for DMA zones. `PmNode::ALLOWED_GAF` strips `Gaf::Dma*` before use, and the zone walk ignores zone type, so the request went nowhere.
- Merge had the opposite polarity to Linux, which merges compatible caches by default and opts out with SLAB_NO_MERGE.
- Reclaim mapped to `Gaf::Reclaim`, which is DirectReclaim plus DaemonReclaim. DirectReclaim means "this allocation may wait", so a cache created with it would have had slab refills that block. Linux's SLAB_RECLAIM_ACCOUNT means something else: it adds `__GFP_RECLAIMABLE`, a hint for which pageblocks to group the slab's pages in.
- Alias was read by nothing, not even by `MemCache::alias()`, which ignores its Option argument and returns Unimplemented.

The mapping also had to go before the Gaf bits it writes can be deleted (step 9).

**Change**
Before:

```cpp
enum class Option {
    Dma = BIT(0),
    Dma32 = BIT(1),
    CacheAlign = BIT(4),
    RedZone = BIT(6),
    Poison = BIT(7),
    Merge = BIT(10),
    Reclaim = BIT(11),
    Alias = BIT(12),
    Folio = BIT(13),
};
...
if (!!(option & Option::Dma)) {
    m_gaf |= Gaf::Dma;
}
// and the same for Dma32 and Reclaim
```

After:

```cpp
enum class Option {
    CacheAlign = BIT(4),
    RedZone = BIT(6),
    Poison = BIT(7),
    Folio = BIT(13),
};
```

The surviving values keep their bits: the whole enum is up for deletion in step 13, so renumbering now would be churn.

**Reference**
- Linux `SLAB_CACHE_DMA`, `SLAB_CACHE_DMA32`, `SLAB_NO_MERGE` and `SLAB_RECLAIM_ACCOUNT` in `include/linux/slab.h`; `calculate_sizes()` in `mm/slub.c` turns SLAB_RECLAIM_ACCOUNT into `__GFP_RECLAIMABLE` in the cache's `allocflags` (from memory).
- Zircon's slab allocator has no creation flags at all; `ObjectCache` takes only a Single/PerCpu template option.

**Verification**
- `boot_check.py` in a scratch worktree at `daf846e7` with this change applied: dbg PASS (24 passed), rel PASS (21 passed).
- `git grep` for the five names as `Option::` or `Flags::` under `kernel/` is empty.
- clang-tidy on `object_cache.cpp`: nothing on the changed lines. clang-format: clean.

**Deliberately not done**
- CacheAlign, RedZone, Poison and Folio: they are steps 7 and 13, with decisions D8 and D10.
- `MemCache::alias()` itself: a stub, decision D8.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the MemCache::Option rows): `git grep` found no caller passing these values.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What would a cache created with the old Option::Reclaim have done on a slab refill, and why is that not what Linux's SLAB_RECLAIM_ACCOUNT means?
- Why did the Option-to-Gaf mapping have to go before the Gaf bits themselves?
- Why were the remaining values left at their old bit positions?

## Title: The slab's red-zone and poison layout goes, since nothing wrote or checked the guard bytes (flag-consolidation step 7) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/object_cache.hpp` — `Option::RedZone`, `Option::Poison`, `m_inuse` and `m_red_left_pad` go; `compute_layout()` loses its Option parameter, whose only readers were the deleted branches
- `kernel/mm/object_cache.cpp` — `CONFIG_MEM_CACHE_DEBUG`, its three branches in `compute_layout()`, and the five `RED_*`/`POISON_*` constants go
- `.agents/tasks/flag-consolidation/DESIGN.md` — D8 decided for this stub

**Symptom**
Latent. No cache was created with RedZone or Poison, so every cache took the non-debug layout and nothing changed at run time. Had one been created with RedZone, `compute_layout()` would have doubled its object size.

**Cause**
The objects, as Linux's SLUB has them:
- A red zone: guard bytes on either side of each object, filled with a known value.
- Poison: a freed object filled with 0x6b, with a different byte at its end.
- The free pointer: SLUB keeps it near the middle of a free object; poisoning overwrites the whole object, so with poisoning on it moves the pointer outside the object.

The events in Linux:
1. `kfree()` checks the red zones; a changed byte is a write past the object, reported there.
2. The next allocation of that object checks the poison; a changed byte is a write after free.

Ours copied only the room-making half: `compute_layout()` grew the object and moved the free pointer for these options. Nothing filled the guard bytes or checked them, the constants were `[[maybe_unused]]`, and `m_inuse` and `m_red_left_pad` were written and never read. `CONFIG_MEM_CACHE_DEBUG` was hard-defined to 1, so the dead branches were compiled into every build. And the one red-zone branch was wrong: Linux adds one word when the object fills its last word,

```c
if ((flags & SLAB_RED_ZONE) && size == s->object_size)
    size += sizeof(void *);
```

where ours added `WORD_SIZE + size`, the object a second time. Half a debugging feature looks like protection and gives none.

**Change**
Before:

```cpp
usize phys_size = ustl::mm::align_up(size, WORD_SIZE);
if constexpr (CONFIG_MEM_CACHE_DEBUG) {
    if (!!(option & Option::RedZone) && phys_size == m_user_size) {
        phys_size += WORD_SIZE + size;
    }
}
m_inuse = static_cast<u32>(phys_size);
if constexpr (CONFIG_MEM_CACHE_DEBUG) {
    if (!!(option & Option::Poison) || (!!(option & Option::RedZone) && m_user_size < phys_size)) {
        m_next_offset = static_cast<u32>(phys_size);
        phys_size += WORD_SIZE;
    } else {
        m_next_offset = ustl::mm::align_down(m_user_size / 2, WORD_SIZE);
    }
} else { ... }
// and a third branch adding m_red_left_pad
```

After:

```cpp
usize phys_size = ustl::mm::align_up(size, WORD_SIZE);
m_next_offset = ustl::mm::align_down(m_user_size / 2, WORD_SIZE);
phys_size = ustl::mm::align_up(phys_size, m_user_align);
```

Every existing cache already took the branch that remains, so its layout is unchanged. Decision D8 offered keeping the scaffolding with only the double add fixed; the user chose to delete it. If slab debugging is wanted, it comes back as a feature with its checks, not as layout alone.

**Reference**
- Linux `calculate_sizes()` in `mm/slub.c` (the layout), and `check_object()`, `init_object()` and `check_bytes_and_report()` there (the half that was never copied); `SLAB_RED_ZONE` and `SLAB_POISON` in `include/linux/slab.h`; the byte values in `include/linux/poison.h` (from memory).
- Zircon's `ObjectCache` (`zircon/kernel/lib/object_cache/object_cache.cc`) has no red zones or poisoning; its heap leaves those checks to KASAN, through the asan hooks in `kernel/lib/heap/cmpctmalloc/cmpctmalloc.cc`.

**Verification**
- `boot_check.py` in a scratch worktree at `b7501a93` with this change applied: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped). The skip counts come from the shell tests at that commit, not from this change.
- `git grep` for `RedZone`, `Poison`, `CONFIG_MEM_CACHE_DEBUG`, `m_inuse` and `m_red_left_pad` under `kernel/` finds only the enum's doc comment.
- clang-tidy on `object_cache.cpp`: nothing on the changed lines. clang-format: clean.

**Deliberately not done**
- `MemCache::init()` still takes an Option it now only forwards to nothing. The whole Option parameter goes in step 13, with CacheAlign and Folio (D10).
- Slab debugging as a real feature. Not asked for.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the RedZone and Poison rows) found no caller and no checker, and compared the layout with Linux's `calculate_sizes()`.

**The user's prediction**
None: a keep-or-delete decision on scaffolding.

**Three questions you should be able to answer**
- What does each of Linux's two halves of slab debugging do, and which half did ours have?
- Why must the free pointer move out of the object when poisoning is on?
- What did the old red-zone branch add to the object size, and what does Linux add?

## Title: Gaf loses the bits PmNode masked off before use, and ALLOWED_GAF goes with them (flag-consolidation step 9) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — Dma, Dma32, Movable, NoFail, NoRetry, Write, DaemonReclaim, Compound and the Reclaim combination go, and `gaf_zone_type()` with its static_asserts; DirectReclaim's doc now says what it means; the presets spell DirectReclaim directly
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp` — `ALLOWED_GAF` and the `gaf &= ALLOWED_GAF` in `alloc_frames()` go; the slow path loses its NoFail and NoRetry branches
- `kernel/mm/include/ours/mm/priv/zone_iterator.hpp` — `AllocationContext::build()` bounds the walk at Normal directly instead of asking `gaf_zone_type()`
- `kernel/mm/object_cache.cpp` — a cache's `m_gaf` starts empty instead of Compound
- `kernel/mm/cpu_local.cpp` — `GAF_GROW` drops NoRetry; its comment says why leaving DirectReclaim out is enough
- `kernel/commands/pm_node_test.cpp` — a comment no longer credits GAF_KERNEL's zone type
- `docs/repairs/260908/heap.md` — item 3 loses one of its two fixes

**Symptom**
None observable: every removed bit was stripped before anything read it, or never read at all.

**Cause**
The objects:
- `Gaf`: the flags a frame allocation is asked with, named after Linux's GFP flags.
- `PmNode::ALLOWED_GAF`: a static mask, set to `GAF_BOOT` (ThisNode, DirectReclaim, DaemonReclaim, Zero) and never changed. `alloc_frames()` did `gaf &= ALLOWED_GAF` first thing.
- `gaf_zone_type()`: turned Dma, Dma32 and Movable into a highest zone type, read only by `AllocationContext::build()`, which only `pmnode.test` uses.

The events of one allocation:
1. A caller passes, say, `GAF_KERNEL`, which carried Write, or a MemCache's `m_gaf`, which carried Compound.
2. `alloc_frames()` masks it with `ALLOWED_GAF`. Dma, Dma32, Movable, NoFail, NoRetry, Write and Compound are not in the mask, so they are gone.
3. The zone walk and the slow path read only DirectReclaim, ThisNode and Zero. DaemonReclaim survived the mask, but nothing read it.

`ALLOWED_GAF` is Linux's `gfp_allowed_mask` with the polarity inverted. In Linux the mask starts at `GFP_BOOT_MASK` and `kernel_init_freeable()` widens it once the scheduler can sleep. Ours was never widened, so the bits outside it were names with no effect. The slow path's comments described NoFail ("retry infinitely") and NoRetry as working, and `GAF_GROW`'s comment relied on NoRetry, when neither could ever arrive.

**Change**
Before:

```cpp
enum class Gaf : u32 {
    Dma = 0_bit, Dma32 = 1_bit, Movable = 2_bit,
    NoFail = 3_bit, NoRetry = 4_bit, Write = 5_bit,
    DaemonReclaim = 6_bit, DirectReclaim = 7_bit, Compound = 8_bit,
    ThisNode = 9_bit, Zero = 10_bit,
    Reclaim = DirectReclaim | DaemonReclaim,
    Boot = ThisNode | Reclaim | Zero,
    User = ThisNode | Zero,
    Kernel = ThisNode | Reclaim | Write,
};
...
gaf &= ALLOWED_GAF;
...
if (!!(gaf & Gaf::NoRetry)) { break; }
if (never_fail) [[unlikely]] { ... task::Thread::Current::reschedule(); }
```

After:

```cpp
enum class Gaf : u32 {
    DirectReclaim = 7_bit,
    ThisNode = 9_bit,
    Zero = 10_bit,
    Boot = ThisNode | DirectReclaim | Zero,
    User = ThisNode | Zero,
    Kernel = ThisNode | DirectReclaim,
};
```

With only DirectReclaim, ThisNode and Zero left, `ALLOWED_GAF` (which held all three) masks nothing, so it goes in the same commit. Deleting it before the bits would have changed behaviour: NoRetry and NoFail would have started to work.

`AllocationContext::build()` used `gaf_zone_type(gaf)` as the walk's upper zone type, which was Normal for every Gaf without Dma bits. It now passes Normal directly, Linux's highest zone for GFP_KERNEL. The bound cannot simply go: `ZoneIterator::upper_zone_type` has no default, so leaving it unset would filter on an indeterminate value.

The survivors keep their bit values: nothing stores a Gaf, and steps 10 to 12 change this enum again.

**Reference**
- Linux `gfp_allowed_mask`, set to `GFP_BOOT_MASK` and widened in `kernel_init_freeable()` (`init/main.c`); `__GFP_DMA`, `__GFP_DMA32`, `__GFP_MOVABLE`, `__GFP_NOFAIL`, `__GFP_NORETRY`, `__GFP_WRITE`, `__GFP_KSWAPD_RECLAIM`, `__GFP_COMP` and `__GFP_RECLAIM` in `include/linux/gfp_types.h`; `gfp_zone()` in `include/linux/gfp.h`, which `gaf_zone_type()` copied (from memory).
- Zircon's PMM takes two flags, `PMM_ALLOC_FLAG_ANY` and `PMM_ALLOC_FLAG_CAN_WAIT` (`zircon/kernel/vm/include/vm/pmm_node.h`): no zone or retry-policy bits. CAN_WAIT is DirectReclaim's nearest counterpart, with one difference: it lets the PMM answer `ZX_ERR_SHOULD_WAIT` and leaves the waiting to the caller, where ours retries inside the allocator.

**Verification**
- `boot_check.py` in a scratch worktree at `b0350f9f` with this change applied: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped).
- `kshell_run.py pmnode.test` on that dbg image: ok, 20 checks, including "ZoneIterator's type bound filters out higher zone types" and "AllocationContext walks the local zones for ThisNode, the fallback otherwise" (2 of 2 zones).
- `git grep` for the removed names, `ALLOWED_GAF`, `gaf_zone_type` and `never_fail` under `kernel/` is empty.
- clang-tidy on `pm_node.cpp`, `cpu_local.cpp` and `pm_node_test.cpp`: nothing on the changed lines (two pre-existing `use nullptr` in the reclaim and compact stubs). clang-format: clean.

**Deliberately not done**
- The endless DirectReclaim retry. NoRetry was one of the two fixes `docs/repairs/260908/heap.md` item 3 offered; with it gone, the remaining fix is giving up without progress, decision D7, a task of its own.
- Gaf::Boot, User and Kernel as enum values, and ThisNode and Zero in the presets: steps 10 to 12.
- AllocationOptions::Kswapd and NoBlock, which duplicate DaemonReclaim and the absence of DirectReclaim: the user kept AllocationOptions (D8, step 8 dropped).

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the Gaf rows) compared each bit with `ALLOWED_GAF` and found which ones reached a reader.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which bits did `ALLOWED_GAF` let through, and why did that make NoFail and NoRetry dead even though the slow path tested them?
- Why did `ALLOWED_GAF` have to go in the same commit as the bits, not before them?
- What would `AllocationContext::build()` have done had its zone bound simply been deleted?

## Title: The Gaf presets become constants outside the enum, and GAF_BOOT gives way to GAF_KERNEL | Gaf::Zero (flag-consolidation step 10) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::Boot`, `Gaf::User`, `Gaf::Kernel` and `GAF_BOOT` go; `GAF_KERNEL` and `GAF_USER` are spelled from the bits, outside the enum
- `kernel/mm/cpu_local.cpp` — the two `GAF_BOOT` allocations ask for `GAF_KERNEL | Gaf::Zero`; three comments follow
- `kernel/mm/init_vmm.cpp`, `kernel/mm/sparse_memmap.cpp` — comments that named GAF_BOOT, or described GAF_KERNEL as ThisNode|Reclaim|Write

**Symptom**
None observable. Two spellings of the same presets: the `Gaf::` forms were never used directly, and the `GAF_*` constants on about a hundred lines were aliases of them.

**Cause**
The objects:
- `Gaf`: the bits of a frame request.
- The presets: `GAF_KERNEL`, `GAF_USER` and `GAF_BOOT`, the combinations callers actually write.

Keeping the presets inside the enum made Gaf two things at once, a set of bits and a list of named combinations, and it made every combination a value of the type, so a switch over Gaf or a printer of its bits would meet them. Linux keeps `GFP_KERNEL` and `GFP_USER` as #defines beside the bit list for that reason.

GAF_BOOT had become exactly `GAF_KERNEL | Gaf::Zero` once step 9 removed Write and DaemonReclaim. "Boot" named a phase, but what its two callers needed was zeroed memory, so the name hid the one bit that mattered.

**Change**
Before:

```cpp
enum class Gaf : u32 {
    DirectReclaim = 7_bit, ThisNode = 9_bit, Zero = 10_bit,
    Boot = ThisNode | DirectReclaim | Zero,
    User = ThisNode | Zero,
    Kernel = ThisNode | DirectReclaim,
};
inline constexpr auto GAF_BOOT = Gaf::Boot;
inline constexpr auto GAF_USER = Gaf::User;
inline constexpr auto GAF_KERNEL = Gaf::Kernel;
...
auto frame = alloc_frames(nid, mm::GAF_BOOT, order);
```

After:

```cpp
enum class Gaf : u32 { DirectReclaim = 7_bit, ThisNode = 9_bit, Zero = 10_bit };
inline constexpr auto GAF_KERNEL = Gaf::ThisNode | Gaf::DirectReclaim;
inline constexpr auto GAF_USER = Gaf::ThisNode | Gaf::Zero;
...
auto frame = alloc_frames(nid, mm::GAF_KERNEL | mm::Gaf::Zero, order);
```

Every preset keeps its bits, so no allocation changes.

**Reference**
- Linux `GFP_KERNEL` and `GFP_USER` in `include/linux/gfp_types.h`, defined from the `__GFP_*` bits; there is no boot preset, only the `GFP_BOOT_MASK` mask (from memory).
- Zircon passes `PMM_ALLOC_FLAG_ANY` or `PMM_ALLOC_FLAG_CAN_WAIT` directly and has no presets.

**Verification**
- `boot_check.py` in a scratch worktree at `048bab4c` with this change applied: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped).
- `git grep` for `GAF_BOOT`, `Gaf::Boot` and `Gaf::Kernel` outside the change history and task documents is empty.
- clang-tidy on `cpu_local.cpp`: nothing on the changed lines. clang-format: clean.

**Deliberately not done**
- ThisNode in the presets (D5, step 11) and Zero in GAF_USER (D6, step 12): both change behaviour and wait for the user.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the Gaf Boot and User/Kernel rows).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a preset inside a bitmask enum make the enum worse, even when its value is right?
- What was GAF_BOOT after step 9, and why is asking for Zero by name clearer than a boot preset?
- Which preset may wait, and which one fails instead?

## Title: No preset carries Gaf::ThisNode any more; the bit stays, documented as strict (flag-consolidation step 11) (2026-10-04)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — `GAF_KERNEL` is DirectReclaim alone and `GAF_USER` is Zero alone; ThisNode gets a doc comment saying it is strict, Linux's `__GFP_THISNODE`, and when to ask for it
- `kernel/mm/cpu_local.cpp` — `GAF_GROW` is Zero alone
- `kernel/mm/pm_node.cpp` — `alloc_frames_preamble()`'s comment says what ThisNode does instead of calling it a preference
- `kernel/mm/sparse_memmap.cpp` — a comment that spelled GAF_KERNEL's bits
- `kernel/commands/pm_node_test.cpp` — the "without ThisNode" case passes `GAF_KERNEL` as is
- `.agents/tasks/flag-consolidation/DESIGN.md`, `PLAN.md` — D5 decided, step 11 narrowed

**Symptom**
Latent on the single-node images. On a NUMA machine every `GAF_KERNEL` allocation, almost every allocation in the kernel, was confined to the node it started from. When that node ran out, the zone walk skipped the other nodes' free memory, and DirectReclaim sent it round the slow path for ever: a hang, not a failure.

**Cause**
The objects:
- `Gaf::ThisNode`: carried by `GAF_KERNEL`, `GAF_USER` and cpu_local's `GAF_GROW`.
- `allowed_nodes`: the nodes one allocation may take frames from.
- `PmNode::m_fallback`: the node's zone list, its own zones first and then the other nodes'.
- `alloc_frames_preamble()`: sets `allowed_nodes` to the caller's mask, and with ThisNode narrows it to this node whenever the mask includes it.
- `get_frame_from_zone_list()`: walks `m_fallback` and skips zones on nodes not in `allowed_nodes`.

The events, on node 0 of a two-node machine, with node 0 empty and node 1 free:
1. The preamble narrows `allowed_nodes` from {0,1} to {0}.
2. The walk reaches node 1's zones and skips them.
3. No frame; DirectReclaim; the reclaim and compaction stubs return nothing; back to 1.

The comment in the preamble called ThisNode "a preference rather than a restriction", because a mask without this node keeps its own nodes. But that branch never runs for the normal masks, which all include this node, so the bit was strict in practice. Its stated reason, that a memoryless node must still allocate, did not hold either: `PmNode::init()` sets every node Online whether or not it has memory, so a memoryless node is in the mask and ThisNode left it nothing.

Local-first, the thing a preset wants, already comes from `m_fallback`'s order. Strictness is for callers to whom memory on another node is useless.

**Change**
Before:

```cpp
inline constexpr auto GAF_KERNEL = Gaf::ThisNode | Gaf::DirectReclaim;
inline constexpr auto GAF_USER = Gaf::ThisNode | Gaf::Zero;
constexpr auto GAF_GROW = Gaf::ThisNode | Gaf::Zero;
```

After:

```cpp
inline constexpr auto GAF_KERNEL = Gaf::DirectReclaim;
inline constexpr auto GAF_USER = Gaf::Zero;
constexpr auto GAF_GROW = Gaf::Zero;
```

Decision D5 offered deleting the bit. The user kept it, strict as Linux's is, for a caller that needs it by name, and asked for an audit of which callers do. By the path each request takes to the PMM:

| Path | Examples | Needs strict? |
|---|---|---|
| Node-less frame allocations | zero page, `alloc_page_table()`, vmalloc | No: placement only |
| VMO pages | `VmCowPages` with `GAF_USER` | No |
| Slab refills | kmalloc, ObjectCache, dispatchers' `new (GAF_USER)` | No: `SlabBody::node_cache` already expects frames from a fallback node |
| CpuLocal units | `build_dense()`, `build_scattered()` | No, see below |
| CpuLocal growth | `GAF_GROW` | No |
| Memmap | `SparseMemMap` | No |

CpuLocal was weighed separately, since per-CPU data is where locality matters most. It still drops the bit: its unit-to-node map is mostly round-robin until CPU enumeration moves ahead of the PMM, so strictness would pin a unit to an arbitrary node, and with `GAF_KERNEL`'s DirectReclaim a node without memory would hang the boot instead of failing it. The only remaining user is `pmnode.test`, which tests the bit itself.

The preamble's empty-intersection branch is kept as code; only its comment now says what it does.

**Reference**
- Linux `GFP_KERNEL` and `GFP_USER` carry no `__GFP_THISNODE` (`include/linux/gfp_types.h`). `__GFP_THISNODE` selects the `ZONELIST_NOFALLBACK` zonelist (`gfp_zonelist()` in `include/linux/gfp.h`), which `AllocationContext::build()` mirrors with `local_zones()`. Linux's percpu allocator, `pcpu_alloc_pages()` in `mm/percpu-vm.c`, uses `alloc_pages_node(cpu_to_node(cpu), gfp, 0)` without it (from memory).
- Zircon's PMM has no NUMA.

**Verification**
- `boot_check.py` in a scratch worktree at `ed96b20a` with this change applied: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped).
- The same with a NUMA build (`mm_numa = true`) under `NUMA=1`, four nodes of 256 MiB each in the memory map: dbg PASS, rel PASS. A first try booted the default build under `NUMA=1` and saw one node, which is why the build argument matters.
- `kshell_run.py pmnode.test`: ok, 56 checks on the NUMA dbg image (the walk without ThisNode covers 5 of 5 zones) and 20 on the single-node one.
- clang-tidy on `pm_node.cpp` and `pm_node_test.cpp`: nothing on the changed lines. clang-format: clean.

**Deliberately not done**
- Deleting the bit: D5, the user kept it.
- Making the empty-intersection branch fail as Linux would: no caller reaches it, and it was not asked for.
- A real per-CPU node map for CpuLocal: it needs CPU enumeration before the PMM.
- The endless DirectReclaim retry: D7, `docs/repairs/260908/heap.md` item 3. Without ThisNode it can no longer be reached while another node has memory, but a machine that is out of memory everywhere still spins.

**How it was found**
The flag audit (`.agents/tasks/flag-consolidation/DESIGN.md`, the ThisNode row) read the preamble against its comment; the per-caller audit came from the user's decision.

**The user's prediction**
The user held that ThisNode's strict behaviour is normal and the bit should stay, and asked which callers really need it, naming CpuLocal. The audit found none. CpuLocal's case rested on locality, which the fallback order already gives; its round-robin unit map and the DirectReclaim hang tipped it, and the user chose to drop it there too.

**Three questions you should be able to answer**
- With ThisNode set, why does a two-node machine with free memory on node 1 hang an allocation that starts on an empty node 0?
- Where does "local node first" come from once no preset carries ThisNode?
- What would a caller that genuinely needs ThisNode look like, and why is CpuLocal not one yet?
