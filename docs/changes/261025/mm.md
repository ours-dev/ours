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

## Title: take_pages() works on a clone: shared pages become its own first, and markers keep the hidden node's pages hidden (2026-10-25)

**Files**
- `kernel/mm/vm_cow_pages.cpp` and header — `take_pages_locked()` takes from a node with a parent; new `shows_ancestor_page_locked()`
- `kernel/mm/include/ours/mm/vm_page_list.hpp` — new `replace_page_with_zero_marker()`
- `kernel/mm/include/ours/mm/vm_object_paged.hpp` — `take_pages()`'s doc
- `kernel/commands/vmo_snapshot_test.cpp` — the take-from-clone checks (9 new)
- `kernel/commands/page_source_test.cpp` — the check that a clone is refused is gone
- `docs/repairs/261025/mm.md` — new, found on the way

**Symptom**
None, step 4.4 of `.agents/tasks/user-pager/`. `take_pages()` answered NotSupported for an object over a node with a parent, so a pager could not hand over content it prepared in a clone (a copy-on-write child of a file's VMO, say).

**Cause**
A clone C under a hidden node H holds only what it wrote; everything else it sees through H, whose pages are shared with every other node below H. Taking from C raises two problems a root never has:
- A page C sees through H is not C's to give away: H's page is still what C's sibling sees. Handing it over would pull it out from under the sibling.
- After the take, C's range must read zero. A slot of C that is empty does not read zero: it shows whatever H has there. So where H has a page, emptying C's slot would make C read H's old bytes, the very content that was just taken.

**Change**
`take_pages_locked()` for a node with a parent, in four steps:

```cpp
change_range_locked(offset, len, RangeChange::Unmap);   // 1. entries first, harmless on failure
for (each page offset)                                   // 2. what C sees through H becomes C's
    if (found.page && found.owner != this) cow_page(at, found);   //    copy, or take if C is the last viewer
pages->reserve(nr_held);                                 // 3. room for every slot, the last failure
for (each slot C holds)                                  // 4. move, allocation-free
    hides = shows_ancestor_page_locked(offset);          //    would H's page show once this is empty?
    page:   hides ? replace_page_with_zero_marker() : remove_page()
    marker: append it; keep it in C if it hides, else remove_zero_marker()
```

Step 2 is what a write does (`cow_page()`): a copy when another node still sees the page, or the page itself, moved out of H, when C is its last viewer; either way the share count is kept. A failure there (OutOfMem) leaves C owning copies of what it saw, which reads the same, so the content has not changed. After step 2, every slot of C where H has a page holds a page of C's own, so step 4 never has to insert a marker into an empty slot, which could allocate: it swaps the page for a marker in place (`VmPageList::replace_page_with_zero_marker()`, the inverse of `replace_zero_marker()`).

A zero marker further up needs nothing: an empty slot over it reads zero already. An offset past C's limit, or with nothing anywhere, is a gap and reads zero too.

**Reference**
Zircon `VmCowPages::TakePages()`: for a node with a parent, it stops at each gap where the parent's content may show, commits it with `LookupCursor::RequireOwnedPage()` ("perform copy-on-write on the whole range so we can then take those pages"), and starts again; its `remove_page_callback` moves each page into the splice list and then puts a `VmPageOrMarker::Marker()` in the slot when the parent has any content there (`parent_has_content`, through `FindInitialPageContentLocked()`). Where it differs:
- Zircon walks only the gaps (`RemovePagesAndIterateGaps()`); this walks every page offset with `find_content()`, a lookup per page through the tree.
- A marker of the node's own stays where it is in Zircon and is a gap in the splice list ("Already zero. Can leave a gap"); here it moves into the list as a marker and stays in the node only if it hides a page. Both read zero at both ends.
- Zircon leaves a marker over any parent content, a marker included; this only over a page, since an empty slot over a marker reads zero already.
- Zircon keeps the pages taken so far when a copy fails part way, for forward progress with a page request; nothing here waits, so it returns the error.

**Verification**
- `vmosnapshot.test`, 238 checks, 9 new. An origin with pages 0 to 2 and a zero marker at 3; a clone five pages long that wrote page 1 and has a marker at 4. Taking all five: pages 0 and 2 come out as copies (frame not the origin's), page 1 as written, page 3 a gap (the marker further up), page 4 a marker; the clone reads zero at all five and holds no page; the origin still reads its three pages and zero. Taking pages 0 to 2 again: three markers come out, and stay. And the origin, the last to see page 0 now, takes it on a write instead of copying it, which shows the share count was dropped.
- Control runs: without step 2, 4 checks fail (gaps instead of copies, and the clone reads the origin's bytes); with no marker kept, 2 fail (the clone reads the origin's bytes after the take). Restored, all pass.
- `pagesource.test` (64), `largepage.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- Walking only the gaps: one `find_content()` per page offset, a cost in the range's width and the tree's depth; a performance item, not measured.
- A take from a node with a page source: still NotSupported, as in Zircon.
- `add_zero_marker_locked()` keeping the share count when it hides an ancestor's page: found when the first version of the test did that by hand and the merge at the end asserted; `docs/repairs/261025/mm.md` item 1.

**How it was found**
None for the feature. The repair item: the first version of the check panicked at its end, `Assertion failed on survivor.is_hidden() || 0 == page->share_count()`, when the clone died and the hidden node merged into the origin; the only marker over a hidden page that no copy accounted for was the one the test had put by hand.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why can take_pages() not hand over a page the clone sees through its hidden parent, and what does it do instead?
- Why does an emptied slot of a clone not read zero, and how does the take make it?
- Why does step 4 never insert a marker into an empty slot, and why does that matter?

## Title: a VMO detaches from its page source, and reports its provider's owner id (2026-10-25)

**Files**
- `kernel/mm/include/ours/mm/vm_object.hpp` — new virtuals `detach_source()` and `source_owner_id()`, nothing and none by default
- `kernel/mm/vm_object_paged.cpp` and header — the two for a paged object, through its node
- `kernel/mm/vm_cow_pages.cpp` and header — new `detach_source_locked()` and `page_source_owner_id()`
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — new `PageProvider::owner_id()` (none by default) and `PageSource::owner_id()`
- `kernel/commands/page_source_test.cpp` — the test provider reports an owner id; the detach checks (7 new)

**Symptom**
None, step 5 of `.agents/tasks/user-pager/`. Two things the pager syscalls need from mm had no entry point:
- `pager_detach_vmo` has to cut a VMO off from its pager: `PageSource::detach()` existed, but nothing above the source could reach it except the VMO's death.
- Every pager syscall that names a VMO has to check that this pager made it (`pager_supply_pages`, `pager_detach_vmo`), or one pager could supply pages into, or detach, another's VMO. Nothing said whose provider a VMO's pages come from.

**Cause**
The page source belongs to the node (`VmCowPages::m_page_source`), and only the node and the object over it can reach it; the object layer had no operation for it. And the provider, the one place that knows who made it, had no way to say so.

**Change**
Detach, from the object down:

```cpp
auto VmObjectPaged::detach_source() -> void {
    Guard guard{m_lock};
    m_cow_pages->detach_source_locked();     // m_page_source->detach(), if there is one
}
```

Under the node's lock, which a supply holds from its check that the source is attached to its last page, so a supply in flight finishes before the detach or finds it. Lock order VmCowPages, then PageSource, as decision D2 has it. What `PageSource::detach()` does was already there: every outstanding request is completed, its waiter retries, `get_pages()` answers BadState and the fault fails; no new request is raised; the provider hears `on_detach()`. Pages supplied before stay, readable and mappable (decision D5, keep), so detaching needs no unmap.

The owner id, from the provider up:

```cpp
virtual auto PageProvider::owner_id() const -> ustl::Option<u64> { return ustl::NONE; }
PageSource::owner_id()               // m_provider->owner_id(); the provider is set once
VmCowPages::page_source_owner_id()   // none without a source; the source is set once
VmObject::source_owner_id()          // none for an object with no source
```

Opaque to mm: the pager's proxy (step 9) will report its pager's koid, and the syscall compares it with the pager handle's. Neither read takes a lock, since neither the node's source nor the source's provider ever changes.

**Reference**
Zircon `VmObjectPaged::DetachSource()` → `VmCowPages::DetachSource()` → `PageSource::Detach()`; and `VmObjectDispatcher::pager_koid()` → `VmObject::GetPageSourceKoid()` → `VmCowPages::GetPageSourceKoid()` → `PageSource::GetProviderKoid()` → `PageProvider::GetKoid()` (none by default), compared with the pager's koid in every `sys_pager_*` that names a VMO (`lib/syscalls/pager.cc`). Where it differs: Zircon's `DetachSource()` unmaps the whole VMO and removes every clean page, so that later faults fail the same way everywhere; decision D5 keeps them here.

**Verification**
- `pagesource.test`, 71 checks, 7 new. A VMO over the test provider reports its owner id, and so does a slice of it; a VMO with no source reports none. With page 0 supplied and a read waiting on page 1: `detach_source()` fails the waiting read (InvArgs, the failed fault) and tells the provider; page 0 still reads its supplied bytes; a fault on page 2 fails at once and asks the provider nothing; a supply is BadState.
- Control: with `detach_source_locked()` doing nothing, the check of the waiting read fails (it times out, and the provider hears no detach). The later checks pass in that run only because the test, when an access does not finish in time, closes the source to free the thread, which detaches it.
- `vmosnapshot.test`, `largepage.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- Unmapping and evicting supplied pages on detach, as Zircon does: decision D5.
- The provider that reports a pager's koid, and the syscalls that compare it: steps 9 and 10.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- After a detach, why does a fault on a supplied page still succeed while one on a missing page fails?
- Why is the owner id read with no lock, when the detach takes the node's?
- What would go wrong if `pager_supply_pages` did not compare the VMO's owner id with the pager's?

## Title: a request withdrawn while another waits on it hands its place to the waiter, through swap_async_request() (2026-10-25)

**Files**
- `kernel/mm/include/ours/mm/page_request.hpp` — `PageProvider::swap_async_request(old, heir)`, and `clear_async_request()`'s contract says it is never called for the request a swap took away
- `kernel/mm/page_request.cpp` — `cancel_request_locked()` swaps instead of clearing `old` and sending `heir`
- `kernel/object/include/ours/object/pager_proxy.hpp`, `kernel/object/pager_proxy.cpp` — `PagerProxy::swap_async_request()`
- `kernel/commands/page_source_test.cpp` — TestProvider's `swap_async_request()`; the withdrawal check now expects nothing new sent
- `kernel/commands/pager_test.cpp` — `check_handover()`, three checks, one per place the withdrawn request can be
- `docs/repairs/261025/mm.md` — item 2 removed

**Symptom**
Before the fix, `pager.test`'s new checks recorded the order of the packets the port handed out:

```text
packet already read:    a second Read@0x0                        (the pager is asked again)
packet still queued:    User packet, then Read@0x1000            (the Read moved behind it)
waiting in the proxy:   Read@0x2000, Read@0x4000, Read@0x3000    (the heir moved behind a later request)
```

All three checks failed. Nothing was wrong in the result: a second supply finds the pages there and gives them back. But the pager did the work twice, or a request lost its place in line. It is reachable from user space: two threads fault on one page, and the first one's wait is cut short by `task_kill`, `task_suspend` or the 300 s overtime.

**Cause**
A primary is the request the provider was sent. An overlap is a request that waits on a primary for the same pages. When a primary is withdrawn while overlaps wait, `cancel_request_locked()` hands its range to the first overlap, the heir, so the pages are still asked for.

It told the provider in two calls: `clear_async_request(old)`, then `send_async_request(heir)`. To the provider those are two unrelated events, "forget this one" and "here is a new one". So whatever it had of `old` was lost:

- **Pager has read the packet**: it is already answering the range, so `clear` finds nothing to take back. `send` then queues a fresh Read for that same range.
- **Packet still on the port**: `clear` takes it back off the port, and `send` queues a new one at the tail, behind every packet queued since.
- **`old` waiting in the proxy's `m_pending`**: `clear` erases it, and `send` appends `heir` at the back.

The provider needed one event that says "the same request, under a new name".

**Change**
One call, made with both requests owned by the provider for its length:

```cpp
// before
m_outstanding.erase(*request);
m_provider->clear_async_request(request);
request->m_provider_owned = false;
...
heir.m_provider_owned = true;
m_provider->send_async_request(&heir);
// after
m_outstanding.erase(*request);
auto const [_, inserted] = m_outstanding.insert(heir);
heir.m_provider_owned = true;
m_provider->swap_async_request(request, &heir);
request->m_provider_owned = false;
```

`PagerProxy::swap_async_request()` looks in the places `clear_async_request()` looks:

- **The packet carries `old`** (`m_active`): `m_active = heir`. The packet already carries the range, so it stays on the port untouched.
- **`old` waits in `m_pending`**: `heir` is inserted before it, and `old` erased.
- **Neither**: nothing. The pager has the range and its supply completes `heir` too. Or the port has lost its handles, and nothing would be read anyway.

Afterwards the proxy may hold `heir` nowhere at all, which is the third case. That is safe: `clear_async_request(heir)` later finds it neither active nor queued and does nothing, as it already did for a request whose packet was read.

**Reference**
Zircon `PageProvider::SwapAsyncRequest()` (`vm/include/vm/page_source.h`), called from `PageSource::CancelRequestLocked()` (`vm/page_source.cc`). `PagerProxy::SwapAsyncRequest()` (`object/pager_proxy.cc`) has the same three cases: `pending_requests_.insert(*old, new_req)` and then erase, or `active_request_ = new_req`, or nothing.

**Verification**
- `pager.test` 23 checks (3 new), `pagesource.test` 71. Both images boot: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing in the changed files.
- Control: the same three checks run before the fix fail, with the packet orders under Symptom. After the fix, each case gives one Read, in place.

**Deliberately not done**
- Withdrawing a request through a real kill of a faulting thread. The checks raise and withdraw requests on the source directly, which reaches the same `cancel_request_locked()` with no timing to arrange.
- Not measured: a cost in time. What is saved is one pager round trip (packet, supply) per withdrawn primary with waiters, and a primary is withdrawn only by a kill, a suspend or an overtime. The checks count packets instead, and the count was the measurement the repair item asked for.

**How it was found**
None. This was an open question of `.agents/tasks/user-pager/DESIGN.md`, recorded as a repair item when the task closed, and fixed at the user's word.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why could the provider not tell, from a clear followed by a send, that the new request asks for the same range as the old one?
- After a swap whose packet the pager had already read, the proxy holds the heir nowhere. Why is that safe when the heir later completes or is withdrawn?
- Why must both requests be provider-owned during `swap_async_request()`, and not one after the other as with a clear and a send?

## Title: map_physical() maps each protection region from its own frames, not from the range's first ones (2026-10-25)

**Files**
- `kernel/mm/vm_mapping.cpp` — `map_physical()` counts each region's frames from `phys_base + (region_base - base)`
- `kernel/commands/vma_phys_test.cpp` — new: `vmaphys.test`, 6 checks; `kernel/commands/BUILD.gn` builds it
- `docs/repairs/261024/mm.md` — removed, its only item fixed

**Symptom**
Latent, cannot be triggered today. A physical mapping whose range has more than one protection maps every region after the first onto the frames at the start of the range. `vmaphys.test` before the fix, two contiguous frames mapped, page 1 made read-only, `map()` over both:

```text
frames at 0x398000; page 0 -> 0x398000, page 1 -> 0x398000
```

Two virtual pages on one frame, with the second frame never mapped. The kernel does not reach it: `map()` of a physical object is called when the mapping is made, while its range is one region, and `protect()` splits the regions afterwards without mapping again.

**Cause**
The loop has two counters that must count from the same place:

- **The virtual addresses.** The coalescer is made with `region_base` and steps on from there itself.
- **The physical addresses.** Each `append()` gives `phys_base + offset`. `phys_base` is the frame of `base`, the start of the whole range, but `offset` restarts at 0 in each region.

In the first region `region_base == base`, so the two agree. In the second region the virtual side has moved on by `region_base - base` and the physical side has not, so each region's pages land on the frames of the range's first pages.

The fix of the object offset two lines above (`m_vmo_ofs + (base - m_base)`) was the same kind of mistake: a distance measured from the wrong origin.

**Change**
```cpp
// before
coalescer.append(phys_base + offset);
// after
auto const region_phys = phys_base + (region_base - base);
...
coalescer.append(region_phys + offset);
```

The other option was to make `offset` run over the whole range, and subtract `region_base - base` for the virtual side. It was rejected: the coalescer already takes the virtual start per region, so the physical side is the one that has to follow it.

**Reference**
Zircon `VmMapping::MapRange()`. It calls `LookupContiguousLocked()` once for each protection range, with that range's own object offset, so each range starts from its own frame.

**Verification**
- `vmaphys.test`, 6 checks: nothing is in the page tables before `map()`; the protect splits the range; `map()` is Ok; page 0 is on the first frame, writable; page 1 is on the second frame, read-only; the address space is destroyed. After the fix: `page 0 -> 0x16e000, page 1 -> 0x16f000`.
- `largepage.test` 27, `vmosnapshot.test` 238, `pagesource.test` 71, `pager.test` 23.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing on the changed lines.
- Control: the same test before the fix fails its page 1 check, with the frames shown under Symptom.

**Deliberately not done**
- A `vma_protect` system call, which would let a program reach this path. Not asked for.
- The pre-existing clang-tidy findings elsewhere in `vm_mapping.cpp`: `MappingCoalescer`'s member names, `0` for null. They are not this change's.

**How it was found**
Reading `map_physical()` while changing `VmMapping::map()`'s options (`docs/changes/261024/mm.md`, the populate fix). It was recorded as a repair item then, and confirmed now by the new test before the fix.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why does the first protection region come out right even with the bug?
- What made the bug unreachable today, and which future change would reach it first?
- The coalescer is given `region_base` but only physical addresses afterwards; why does that force the physical side to start from the region too?

## Title: commit_range() of a VMO with a page source waits for the pages, with its lock let go, and goes on from where it stopped (2026-10-25)

**Files**
- `kernel/mm/vm_object_paged.cpp` — `commit_range_internal()` loops: commit, and on ShouldWait wait on the request outside the lock and commit again from the first page not yet committed
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `commit_range_locked()` takes the caller's `MultiPageRequest` and answers ShouldWait instead of asserting it never happens
- `kernel/commands/page_source_test.cpp` — a `VmoCommit` access, and a commit of pages 7 and 8 of the sourced VMO (now 9 pages long), 5 checks
- `docs/repairs/261023/mm.md` — item 1 removed

**Symptom**
Latent: nothing in the kernel commits a sourced VMO, and no system call commits a VMO. `pagesource.test`'s new check, before the fix, panicked the debug kernel:

```text
OURS KERNEL PANIC
Assertion failed on status != Status::ShouldWait
```

**Cause**
For a page that is not there yet, a sourced VMO's cursor raises a read request and answers ShouldWait. The caller has to wait on that request with the VMO's lock let go, because the provider supplies the page under that same lock.

`copy_page_by_page()` has worked this way since 2026-10-23 (`docs/changes/261023/mm.md`). The commit path was still written for anonymous VMOs, where ShouldWait cannot happen:

- **The request lived inside the locked call.** It was a local of `commit_range_locked()`. Even if ShouldWait had been returned, the request would have been withdrawn by its destructor before anyone could wait on it.
- **The call asserted ShouldWait away.** `DEBUG_ASSERT(status != Status::ShouldWait)`, which a release build skips; there, the commit returned ShouldWait to a caller that does not expect it.
- **There was no loop.** `commit_range_internal()` made one call under the lock and returned whatever it said. Its comment noted that the loop had to come back "whole -- the wait, the advance, the retry -- not the shape of it", the day something commits a sourced VMO.

**Change**
```cpp
// before: one call, the request a local of commit_range_locked()
Guard guard{m_lock};
usize nr_committed = 0;
return m_cow_pages->commit_range_locked(m_cow_offset + offset, size, &nr_committed);

// after
MultiPageRequest request;
usize done = 0;
while (done < size) {
    {
        Guard guard{m_lock};
        // range check, rechecked on every pass
        usize nr_committed = 0;
        auto const status = m_cow_pages->commit_range_locked(
            m_cow_offset + offset + done, size - done, &request, &nr_committed);
        done += nr_committed * PAGE_SIZE;
        if (Status::ShouldWait != status) {
            return status;
        }
    }
    if (auto const status = request.wait(true); Status::Ok != status) {
        return status;
    }
}
```

These are the three parts the old comment asked for:

- **The wait**: on the request, with no lock held. Suspendable, as the copy's wait is.
- **The advance**: by the pages the last call committed, so the next call starts at the page that was missing, not at the start of the range.
- **The retry**: from the top, with the range checked again under the lock.

A wait that ends without the page fails the commit with the wait's status. The pages committed before it stay committed.

**Reference**
Zircon `VmObjectPaged::CommitRangeInternal()`. It raises the page request under the lock, drops the lock to wait on it, and loops with the offset advanced past what was committed.

**Verification**
- `pagesource.test`, 76 checks (5 new):
  - The commit asks for page 7.
  - Once page 7 is supplied, it goes on from page 8 and asks for page 8, not for 7 again.
  - It returns Ok, with both pages committed.
- `pager.test` 23, `vmosnapshot.test` 238, `largepage.test` 27.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing in the changed lines.
- Control: the same check before the fix panics the kernel, as shown under Symptom.

**Deliberately not done**
- Committing more than one page per request: that is repair item 2 of `docs/repairs/261023/mm.md`, a performance item that waits for the user's decision.
- `CommitOptions::Pin` on a sourced VMO: no caller pins one, and pinning here is still only a check of the size.

**How it was found**
None. It was recorded on 2026-10-23, when the copy paths learned to wait (`docs/changes/261023/mm.md`), and fixed now in the user's round of the remaining repairs.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why must the request be declared outside the locked block, and what would its destructor do if it were a local of `commit_range_locked()`?
- Why does the next call start at `offset + done` and not at `offset`, and what would the test have shown without the advance?
- Why is the wait made with the VMO's lock let go, and who needs that lock while this thread waits?

## Title: a zero marker put over a hidden node's page stops the node viewing it, and the page's count drops (2026-10-25)

**Files**
- `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `stop_viewing_locked()`, taken out of `release_viewed_pages_locked()`; `add_zero_marker_locked()` calls it for the page its marker hides
- `kernel/commands/vmo_snapshot_test.cpp` — a marker on a clone over its hidden node's page, then the clone's death and the merge, 3 checks
- `docs/repairs/261025/mm.md` — removed, its last item fixed

**Symptom**
Latent: only tests put a marker on a clone today. Supplied markers go into a root with a page source, which has no ancestors. `vmosnapshot.test`'s new check before the fix:

```text
the page's count: 1 before the marker, 1 after
OURS KERNEL PANIC
Assertion failed on survivor.is_hidden() || 0 == page->share_count() with message: a page with another viewer in a hidden node with one child
```

**Cause**
A page in a hidden node counts how many leaves below it can see it (`share_count()`, viewers less one). Every path that stops a leaf seeing such a page keeps the count:

- a write's copy (`cow_page()`);
- a leaf's death (`release_viewed_pages_locked()`);
- `take_pages_locked()`, which only puts a marker where the leaf already holds a copy of its own.

`add_zero_marker_locked()` put a marker wherever it was told and did not look up. A marker on a clone, over a page of the hidden node above it, hid that page from the clone without counting the clone out.

The count stayed one too high. When the clone died, the hidden node had one child left and merged into it. The merge found a page that still claimed a second viewer, and stopped the kernel. In a release build, where the assert is skipped, the page moves into the survivor still carrying the wrong count. Every later decision that reads the count about that page, copy or take, free or keep, starts from a number that is off by one.

**Change**
The repair item offered two fixes: drop the share in `add_zero_marker_locked()`, or refuse such a marker with a `DEBUG_ASSERT` until a caller needs one. This change takes the first. A caller that needs it is already in sight: zeroing a range of a clone (Zircon's `ZeroRange`, a later `vmo_op_range`) puts exactly such markers.

The drop-or-free step of `release_viewed_pages_locked()` became `stop_viewing_locked(found)`, used by both:

```cpp
// add_zero_marker_locked(), after
FoundContent hidden{};
if (m_parent && m_pages.lookup(PgOff(index)).is_empty()) {
    auto const found = find_content(u64{index} * PAGE_SIZE);
    if (found.page && found.owner->is_hidden()) {
        hidden = found;
    }
}
auto const status = m_pages.insert_zero_marker(index);
if (Status::Ok == status && hidden.page) {
    stop_viewing_locked(hidden);
}
```

The page is looked up before the marker goes in, since afterwards the node sees the marker. The share is given up only once the marker is in, since a marker that could not be put (OutOfMem, AlreadyExists) changes nothing.

**Reference**
Zircon keeps the count on every path that hides parent content: `VmCowPages::DecrementCowContentShareCount()`, called among others from `SupplyPagesLocked()` when a slot that showed a hidden ancestor's page is filled.

**Verification**
- `vmosnapshot.test`, 241 checks (3 new):
  - a node and its clone see one page of the hidden node, with a count of one;
  - a marker on the clone drops the count to zero, and the node still reads the page;
  - after the clone dies, the hidden node merges into the node, and the page becomes the node's own, writable and unchanged.
- `pagesource.test` 76, `pager.test` 23, `largepage.test` 27.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing in the changed lines.
- Control: the same check before the fix panics the kernel, as shown under Symptom.

**Deliberately not done**
- A check of the last-viewer case through a marker: the marker's node was the only viewer, so the page is freed. It goes through the same `stop_viewing_locked()` that every leaf's death goes through, which the existing checks cover.

**How it was found**
On 2026-10-25, when the first version of the take-from-clone check in `vmosnapshot.test` put such a marker and the kernel panicked at the end of the check. It was recorded as a repair item then, and fixed now.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why does the count have to drop when a marker hides the page, and not only when the node dies?
- Why is the page looked up before the marker is inserted, but its share given up only after?
- What does a count left one too high do in a release build, where the merge's assert is skipped?

## Title: MappingCoalescer's members follow the m_ convention, and vm_mapping.cpp passes nullptr, not 0, for a pointer (2026-10-25)

**Files**
- `kernel/mm/vm_mapping.cpp` — `mapping_`, `va_`, `pa_`, `nr_pages_`, `total_mapped_` and `mmuf_` are `m_mapping`, `m_va`, `m_pa`, `m_nr_pages`, `m_total_mapped` and `m_mmuf`; four `unmap(..., 0)` calls pass `nullptr`; the class comment says what it does

**Symptom**
None at run time. clang-tidy reported six `readability-identifier-naming` and four `modernize-use-nullptr` findings in the file, and the user pointed at the members.

**Cause**
`MappingCoalescer` predates the repo's naming rule: members are `m_`-prefixed, as every other class in `kernel/mm` has them. The four `unmap()` calls pass the literal `0` for their `usize *unmapped` out-parameter, where the rule is `nullptr`. Its comment, "This helper class was used to batch mapping requests", was in the past tense and named nothing it does.

**Change**
A rename of six members, `0` to `nullptr` in four calls, and the class comment rewritten. Nothing else changes.

`m_total_mapped` is written and never read. It is left in place: whether it is scaffolding for something planned is the user's to say, not a cleanup's.

**Reference**
None, this project's own conventions (`modern-cpp-development`).

**Verification**
- clang-tidy: no finding left in `vm_mapping.cpp`.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.

**Deliberately not done**
- Removing `m_total_mapped`: raised with the user instead.

**How it was found**
The user selected the members and said they do not follow the coding style.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does the repo prefix members with `m_` rather than end them with `_`?
- What does `nullptr` say about an argument that `0` does not, to a reader and to overload resolution?
- What would `m_total_mapped` have to be read by to earn its place?
