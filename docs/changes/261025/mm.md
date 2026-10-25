# mm changes

## Title: pages move between VMOs through a splice list: take_pages() from a node with no parent, supply_pages() of what it took (2026-10-25)

**Files**
- `kernel/mm/include/ours/mm/vm_page_splice_list.hpp`, `kernel/mm/vm_page_splice_list.cpp` — new: `VmPageSpliceList`, a run of page slots in order, each a page, a zero marker or a gap
- `kernel/mm/vm_cow_pages.cpp` and header — new `take_pages_locked()`; `supply_pages_locked()` takes a splice list, and a zero marker or a gap goes in as a zero marker
- `kernel/mm/vm_object_paged.cpp` and header — `take_pages()` implemented; `supply_pages()` takes a splice list
- `kernel/mm/include/ours/mm/vm_object.hpp` — the two virtuals take a `VmPageSpliceList *`
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — `Slot` lets the splice list make slots; `types.hpp` declares the class; `kernel/mm/BUILD.gn` builds it
- `kernel/commands/page_source_test.cpp` — the supplies go through a splice list; the hand-over checks (11 new)

**Symptom**
None, step 4.3 of `.agents/tasks/user-pager/`. A user pager hands over content by preparing it in an ordinary VMO of its own and moving that VMO's pages into the pager-backed one (`pager_supply_pages(pager_vmo, offset, len, aux_vmo, aux_offset)`). The kernel had no way to take pages out of a VMO (`take_pages()` was `Unimplemented`), and `supply_pages()` took a `VmPage::List`, which cannot carry what an aux VMO holds.

**Cause**
What a range of a VMO holds is a sequence of slots, and each slot is one of three things (`VmPageMap::Slot`): a page, a zero marker (known zero, no frame), or nothing. A `VmPage::List` is only frames: it has no positions. An aux VMO with data at pages 0 and 3, a marker at 1 and nothing at 2 would be the list [A, D], and nothing says that D belongs at page 3, or that pages 1 and 2 are zero. The only way to hand such a range over with a list of frames was to make every slot a real page first, allocating and zeroing frames for content that is zero. A splice list keeps one slot per page and keeps their order, so the position in the list is the offset the slot lands at.

Three rules decide how the slots land, each for a reason:
- A gap lands as a zero marker, not as an empty slot. The same "nothing" means two things in the two VMOs: in the anonymous aux VMO an empty slot reads as zero; in the pager-backed VMO an empty slot means "ask the pager". Left empty, the next read would ask the pager for content it has just supplied, and a read already waiting on that page would never be woken, since nothing was supplied there.
- A slot that already holds a page or a marker keeps it, and the arriving page is freed. The content there is what the source supplied earlier; overwriting it under a mapping would change bytes a program already read.
- The source hears of each run of slots filled (`on_pages_supplied()`), markers included: a request waiting on a page that arrived as a marker is complete, since its content is known.

And taking has to be all or nothing, and has to unmap first:
- A slot taken out of the page map cannot be put back if a later step fails, so the list makes room for every slot that moves (one `reserve()`) before the first one does. OutOfMem leaves both the object and the list as they were.
- A frame that leaves the object must leave no entry behind in any mapping of it, or a program keeps reading and writing a frame that now belongs to another VMO. The range is unmapped through the walk of 4.2 (`change_range_locked(..., RangeChange::Unmap)`) before anything moves, under the node's lock, which every fault takes to find a page, so none can map it again in between.

**Change**
The list (`VmPageSpliceList`): sparse, a sorted `ktl::Vec` of the slots that hold something, each with its index in the run; a gap is an index with nothing stored. Filled once in order, emptied once in order:

```cpp
mm::VmPageSpliceList pages(4);           // a run of 4 slots
aux->take_pages(0, 4 * P, &pages);       // [A, marker, gap, D]; aux holds nothing in the range
dst->supply_pages(0, 4 * P, &pages, mm::VmObject::SupplyOption::Default);
```

It owns the pages it holds: whatever was never popped is freed with it, so a supply that stops part way, or a list nobody supplies, loses no frame.

`VmCowPages::take_pages_locked()`:

```cpp
if (m_page_source || m_parent) return Status::NotSupported;
// count the slots that hold something, then pages->reserve(count) -- the only failure
change_range_locked(offset, len, RangeChange::Unmap);
// move each page (remove_page + append_page) and marker (remove_zero_marker + append_zero_marker)
```

`VmCowPages::supply_pages_locked()`, per slot popped: a page goes in with `add_new_page_locked()` and is freed if the slot was full; a marker or a gap goes in with `add_zero_marker_locked()`.

`VmObjectPaged::take_pages()` checks alignment, that the list is fresh and as long as the range (InvArgs), the range (OutOfRange), and refuses a Pinned object (BadState), as the clone does: pinning is the object's, and the kernel's mappings of a pinned object rely on its pages staying where they are.

Rejected: a dense list (one entry per page): a take of 1 GiB would allocate 2 MiB of slots, almost all of them gaps. A `VmPageMap` inside the list: every insertion allocates, so a take could fail half way with slots already out of the object.

**Reference**
Zircon `VmPageSpliceList` (`vm/include/vm/vm_page_list.h`), filled by `VmObjectPaged::TakePages()` → `VmCowPages::TakePages()` and emptied by `VmObjectPaged::SupplyPages()` → `VmCowPages::SupplyPagesLocked()`; `zx_pager_supply_pages()` (`lib/syscalls/pager.cc`) is exactly `TakePages()` on the aux VMO then `SupplyPages()` on the pager VMO. `VmCowPages::TakePages()` refuses a node with a page source (`ZX_ERR_NOT_SUPPORTED`) and pinned pages (`ZX_ERR_BAD_STATE`), unmaps the whole range first (`RangeChangeUpdateLocked(range, RangeChangeOp::Unmap)`), and for a node with no parent moves whole page-list nodes into the splice list (`AddPagesFrom()`), which allocates nothing. `SupplyPagesLocked()` walks the list with `RemovePagesAndIterateGaps()`, inserts a marker for each gap into a pager-backed node ("we need to insert explicit markers to actually resolve the pager fault"), keeps what a slot already has (`CanOverwriteSlot::Empty`), and calls `OnPagesSupplied()` per run. Where this differs: the list is a vector of held slots rather than a page list, so a take makes room with one allocation instead of moving nodes; pinning is checked on the object, since pages carry no pin count here.

**Verification**
- `pagesource.test`, 65 checks, 11 new. An anonymous object holding a page at 0, a zero marker at 1, nothing at 2 and a page at 3, mapped with Populate; an object with a source, page 3 supplied, and a read waiting on its page 2. `take_pages()` of the four pages leaves the anonymous object nothing (0 committed) and its mapping no entry; `supply_pages()` of them succeeds; the waiting read wakes and reads 0; page 0 has the bytes it left with, pages 1 and 2 read 0 with 2 pages committed in all, page 3 keeps the first supply's pattern, and the provider is asked nothing more; page 0, faulted in, is the very frame the anonymous object had. Refused: a take from an object with a source and from a clone (NotSupported), a list of the wrong length (InvArgs), a range past the end (OutOfRange), a pinned object (BadState).
- The supplies the test made before go through a splice list now; their checks pass unchanged.
- Control runs: with a gap skipped instead of becoming a marker, the waiting read is never woken and the hand-over check fails (2 checks); with the take not unmapping first, the aux mapping keeps its entries (1 check). Restored, all pass.
- `vmosnapshot.test`, `largepage.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- A take from a node with a parent: a slot it holds nothing at may show a parent's page, which has to be copied before it can be taken, and a marker has to stay where the parent has content. Step 4.4.
- `pager_supply_pages` itself, the syscall that pairs the two calls: step 10.
- Zircon's unmap of the supplied range in `SupplyPagesLocked()`: it exists for nodes whose empty slots can be mapped (parent content markers, a zero page); an empty slot of a node with a source here is never mapped, since a fault on it waits.
- `SupplyOption::TransferData` (overwrite what is there): still NotSupported.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why does a gap in the splice list have to arrive as a zero marker and not as an empty slot?
- Why does take_pages() unmap the whole range before it moves a single slot, and what lock keeps a fault from mapping one back?
- Why is the room for the list made before anything moves, and what would a failure half way through leave behind otherwise?

## Title: VmPageMap is VmPageList, in vm_page_list.hpp (2026-10-25)

**Files**
- `kernel/mm/include/ours/mm/vm_page_map.hpp` → `vm_page_list.hpp` — the class, its include guard, and its comments, which call it a list now
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `vm_page_splice_list.hpp`, `vm_object_paged.hpp`, `pm_frame.hpp`, `kernel/mm/vm_cow_pages.cpp`, `vm_page_splice_list.cpp` — the new name, and "page list" where the prose said "page map"
- `docs/repairs/260903/mm.md`, `docs/repairs/260930/mm.md`, `docs/repairs/261002/launch.md`, `.agents/tasks/user-pager/DESIGN.md`, `PLAN.md` — the new name; `260930` also had `next_page()`, which is `next_slot()` now, and `261002` a line range that moved

**Symptom**
None, a rename the user asked for: the class is Zircon's `VmPageList` in all but name, and the splice list beside it is Zircon's `VmPageSpliceList`, so the pair now reads as it does there.

**Cause**
None, a name only.

**Change**
`VmPageMap` → `VmPageList`, `VmPageMap::Slot` → `VmPageList::Slot`, `VmPageMap::Tag` → `VmPageList::Tag`, `<ours/mm/vm_page_map.hpp>` → `<ours/mm/vm_page_list.hpp>` (moved with `git mv`). No behaviour changes. Change records keep the old name: they are history, and say what the code was called then.

The one name close to it is `VmPage::List`, the intrusive list of frames (a free list, a batch the allocator hands back): a list of pages with no positions, where `VmPageList` is the slots of one object, keyed by page index.

**Reference**
Zircon `VmPageList` (`vm/include/vm/vm_page_list.h`), the page store of a `VmCowPages`, beside `VmPageSpliceList` in the same header.

**Verification**
- Build, clang-format: clean.
- `vmosnapshot.test`, `largepage.test`, `pagesource.test`, `usercopy.test` pass.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- Change records and finished tasks' documents keep `VmPageMap`.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- What does `VmPageList` hold that `VmPage::List` cannot?
- Why does a change record keep the old name?
- Which three things can one slot of a `VmPageList` be?
