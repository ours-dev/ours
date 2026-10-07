# mm outstanding repairs

## 2. The buddy indexes a zone's descriptors as one array, which the sparse memmap does not give it

**Severity**
Medium: wrong today on every free of a frame outside the section that holds its zone's first pfn, but its only effect so far is lost coalescing, which no boot or test looks at; the arithmetic it rests on is undefined behaviour.

**Symptom**
Blocks freed into a zone above its first 128 MiB section never merge with their buddies, so a max-order block split by an allocation there never comes back together. Nothing fails at boot. A probe that asserts `FrameBuddy::contains(frame)` in `PmZone::free_frame()` stops the rel image at the handoff: `zone DMA32 [0x1000, 0x1fedc) frees pfn 0x8000 at 0xffffff801f4ed000 outside its buddy array (base desc 0xffffff801f72d000)`. On the default 512 MiB image that is every pfn from 0x8000 to 0x1FEDC, three quarters of the memory.

**Cause**
The objects:
- `FrameBuddy` holds `m_frames`, the descriptor of the zone's first pfn, and finds every other descriptor by offset from it: `pfn_of(frame)` is `m_base_pfn + (frame - m_frames)` and `frame_of(pfn)` is `&m_frames[pfn - m_base_pfn]`. That treats the zone's descriptors as one array.
- `PmZone`'s constructor binds the buddy to `MemMap::get().pfn_to_frame(start_pfn)` (since `docs/changes/261007/mm.md`, a zone binds its frame set in its constructor; the lazy attach bound the same pointer).
- The sparse memmap, the default model, gives each terminal section its own descriptor array, allocated separately in `SparseMemMap::activate_section()`: 32768 descriptors of 64 bytes, 2 MiB, per 128 MiB section.

On the default image the four arrays of the DMA32 zone's sections are allocated top-down, 2 MiB apart (the dbg log's section dump gives sections 0 to 3 their arrays at `FFFFFF801F6C0000`, `FFFFFF801F4C0000`, `FFFFFF801F2C0000` and `FFFFFF801F0C0000`), so section 1's array lies below section 0's. For a frame in section 1 or above, `frame - m_frames` subtracts pointers into two different allocations, which C++ leaves undefined; here it comes out negative, the pfn wraps to near 2^64, and `frame_of()` of its buddy finds the index past `m_count` and returns null. So every lookup misses and the block goes on the list unmerged. That the misses are harmless is luck of the layout: with arrays allocated upward and a gap between them (a section-table level allocated in between), `frame_of()` would return a pointer into whatever lies past section 0's array, and `release_inner()` would test and unlink it as a free buddy. The virtually-flat memmap does lay a zone's descriptors out as one array, which is presumably the model the buddy was written against.

`release()`'s walk for orders above `MAX_ORDER` and `FrameBuddy::contains()` make the same assumption. The handoff never frees above `MAX_FRAME_ORDER` (`free_frames_pfn_range()` caps it), and nothing in the kernel calls `contains()`, so neither is reached today.

**Change**
To decide with the user, since it changes the seam that keeps `FrameBuddy` host-testable:

- Recommended: find neighbours relative to the freed descriptor, not from a zone-wide base. The zone passes the frame's pfn (it has `MemMap::frame_to_pfn()`), and the buddy is `frame + (buddy_pfn - pfn)`, which stays inside one section's array because a max-order block never crosses a section: `MAX_FRAME_ORDER + PAGE_SHIFT` (22) is at most `SECTION_SHIFT` (27), to be pinned with a `static_assert`. The zone's span check replaces `m_count`. The host test keeps working by passing pfns it knows.
- Or: one `FrameBuddy` per contiguous descriptor run, several per zone. Correct, but the zone then has to find the run for every free and walk several sets for every allocation.

Ruled out: making the boot allocator place a zone's section arrays adjacently and in order. It depends on allocation order and is still pointer arithmetic across separate allocations.

**Reference**
Linux finds the buddy relative to the page being freed: `find_buddy_page_pfn()` (`mm/internal.h`) computes `__find_buddy_pfn(pfn, order)` and returns `page + (__buddy_pfn - pfn)`, called from `__free_one_page()` (`mm/page_alloc.c`); `include/linux/mmzone.h` refuses a build where `MAX_PAGE_ORDER + PAGE_SHIFT` exceeds `SECTION_SIZE_BITS`, which is what makes that offset stay inside one section's `mem_map`. Zircon gives each `PmmArena` one contiguous `page_array_` for exactly its range and indexes only within it (`get_page()`, `get_index()` in `kernel/vm/include/vm/pmm_arena.h`).

**Acceptance**
A debug check in the zone's free path that the buddy's computed pfn equals `MemMap::frame_to_pfn(frame)` passes through the handoff and a full `boot_check.py`; a kshell test frees the two halves of a split max-order block above pfn 0x8000 and finds one max-order block again; `boot_check.py` passes on both images, and on a `NUMA=1` build with `mm_numa = true`.
