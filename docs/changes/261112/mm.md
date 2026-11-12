# mm changes

## PageFrame::init() writes the order field (2026-11-12)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `init()` sets `Index::Order` to 0; the comment on the Flags write names the field it is about
- `docs/repairs/261026/mm.md` — item 5 removed
- `docs/repairs/261112/mm.md` — new item 1: the rest of the descriptor still needs zeroed storage

**Symptom**
Latent, cannot be triggered today. The order field of a frame nothing had allocated read 0 only because the memmap's storage is allocated with `Gaf::Zero`.

**Cause**
The order field of a frame descriptor has two writers and, until now, one kind of reader. The buddy writes it when it puts a block on a free list and again when it splits one off for a caller (`FrameBuddy::insert_frame()`, `acquire_inner()`); `FrameBuddy::is_mergeable()` reads it, and only on a frame whose role says it is on a free list, so it only ever read a value the buddy had written. A frame that had never been through the buddy carried whatever its storage held, and nobody looked.

The next change makes somebody look. `free_frames()` is about to read the order from the head frame to learn how much is being returned. Most frames it will be given were allocated, and carry the order the buddy stamped. But a frame taken from the boot allocator and later freed as a single page has never been stamped. For it, "whatever the storage held" becomes the size of the block handed to the buddy. Zero is the right answer, and it has to be an answer `init()` gives, not a property of the allocation call in `sparse_memmap.cpp`.

`init()` already wrote five of the six fields of the status word. Its comment on the last write, "The one write of the whole field: whatever the descriptor held before is garbage", was about the Flags field and read as though it covered the word.

**Change**

```cpp
// after, in PageFrame::init()
m_status.set<Index::Role>(Role::None);
m_status.set<Index::Order>(0);
// The one write of the whole Flags field: whatever it held before is garbage.
m_status.set<Index::Flags>(Flags::Initialized);
```

**Reference**
Linux `__init_single_page()` in `mm/mm_init.c` sets every field of `struct page` it is responsible for and does not rely on the allocator having zeroed it. The order of a free block lives in `page_private()`, written by `set_buddy_order()` when the block is freed into the buddy; the order of an allocated compound page lives in the folio, written by `prep_compound_page()`.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed.

The repair item's acceptance test was run and does not pass: with `Gaf::Zero` removed from the memmap allocation, dbg panics in `init()` on its own "Duplicated to initialize a frame" check, which reads the Flags field before writing it, and rel traps at an assertion in `FrameBuddy::release()`. Neither failure involves the order field. They show that the rest of the descriptor depends on zeroed storage too, which is recorded as `docs/repairs/261112/mm.md` item 1.

**Deliberately not done**
The rest of the descriptor (the Flags read, the list hook, the reference and compound words, the role body). Making `init()` a full constructor changes what every memmap allocation may assume and is its own item.

**How it was found**
Recorded on 2026-10-26 as item 5 of `docs/repairs/261026/mm.md`. It became necessary when `free_frames()` was about to make the head frame's order its only source.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Who writes a frame's order field, and why was it harmless that `init()` did not, until `free_frames()` reads it?
- The acceptance experiment failed. Why does that not mean this change is wrong, and what does it mean?
- Why is "the allocation zeroes it" a weaker guarantee than "`init()` writes it", when both give the same bytes today?

## free_frames() takes the block and nothing else (2026-11-12)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp` — `free_frames(PmFrame *)` and `free_frames(VirtAddr)` lose their `order` parameter; the comment says what is returned and how part of a block is returned
- `kernel/mm/pmm.cpp` — `free_frames()` reads the order from the head frame; the assertion of the last step goes with the parameter it compared; `free_page_table()` asserts its size against the block's order
- `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/vm_object_paged.cpp`, `kernel/mm/object_cache.cpp`, `kernel/mm/page_request.cpp`, `kernel/mm/vm_page_splice_list.cpp`, `kernel/arch/x86/kernel/bootstrap.cpp` — 14 production calls
- `kernel/tests/pm_node_test.cpp`, `memory_pressure_test.cpp`, `vma_phys_test.cpp`, `page_reclaim_test.cpp`, `large_page_test.cpp`, `page_source_test.cpp`, `resource_test.cpp`, `vmo_snapshot_test.cpp` — 37 test calls

**Symptom**
Latent, cannot be triggered today. A caller passing the wrong order to `free_frames()` would have returned too little of a block, leaking the rest, or too much, giving the buddy frames that belong to someone else. The last two steps showed no caller does.

**Cause**
A block had two records of its size. The buddy writes the order into the head frame when it hands the block out, and the caller was asked to remember the same number and say it again on the way back. Two records of one fact can disagree, and the interface had no way to notice: `free_frames(frame, order)` believed its argument.

The caller's copy was also the weaker of the two. The allocator knows the order because it made the block. The caller knows it only by carrying a variable from the allocation to the release, sometimes across a data structure and a lock, and the object cache went as far as recomputing it from the cache's layout.

**Change**
The block says how big it is:

```cpp
// before
auto free_frames(PmFrame *frame, usize order = 0) -> void;
mm::free_frames(slab, order);

// after
auto free_frames(PmFrame *frame) -> void;
mm::free_frames(slab);
```

Inside, `free_frames()` reads `frame->order()` and passes it down. The node's and the zone's `free_frames(frame, order)` keep the parameter, because `adopt_frames()` gives them memory that has no head to read.

Returning part of a block is still possible and now has one spelling: make the parts blocks first. `VmObjectPaged::create_contiguous()` and `VmCowPages::fill_block_locked()` already did, setting the order of every frame of the block to 0 before freeing any of them alone; before this change that was a courtesy, and now it is what makes the frees correct.

51 calls lost their second argument. They were rewritten by a script that finds the call, splits its arguments at the top level and keeps the first; it skips member calls, so the node's and the zone's are untouched. Every call it could have missed fails to compile, since no overload takes two arguments any more.

**Reference**
Zircon `pmm_free_page(vm_page_t *)` and `pmm_free(list)`: a page, with no size. Linux went the other way and kept `__free_pages(page, order)`, with `folio_put()` as the form that reads the order from the folio; the two-argument form is the one its documentation warns must be given the order of the allocation.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed. The same on four nodes (`NUMA=1`); both images boot with `MEM=3072`. `ktest all` on dbg, default and four-node: 61 ran, 59 passed, 2 skipped, as before. `format_cpp.py` left every changed file as it was; clang-tidy reports nothing on the changed lines of the seven production files.

What these runs cannot show is a frame freed alone that still carries the order of a block it once headed. The assertion of the last step looked for exactly that for as long as there was an argument to compare, and found none over the same runs.

**Deliberately not done**
- `split_block()`. The two callers that split a block still do it by hand, a loop of `set_order(0)`. One function that splits, and is the only way to, is what would make a stale order on a frame freed alone impossible and not merely absent; it comes with the contiguous shape.
- The node's and the zone's `free_frames()` keep `order`; see Change.
- Tail frames are still stamped by the object cache and not by the allocator.

**How it was found**
None. Decision F2 of `.agents/tasks/pmm-redesign/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A block had two records of its order. Which one could be trusted, and why?
- `create_contiguous()` frees the frames of one block one at a time. What must it do first, and what would go wrong now if it did not?
- Why do `PmNode::free_frames()` and `PmZone::free_frames()` still take an order?

## split_block() is the one way to make parts of a block (2026-11-12)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `split_block(PmFrame *frame, usize order = 0)`
- `kernel/mm/vm_object_paged.cpp` — `create_contiguous()` calls it where it looped over `set_order(0)`
- `kernel/mm/vm_cow_pages.cpp` — `fill_block_locked()` the same
- `kernel/tests/pm_node_test.cpp` — `PmNode.SplitBlockMakesBlocksThatAreFreedAlone`

**Symptom**
Latent, cannot be triggered today. A caller that frees the frames of a block one at a time without first giving each its own order would, since `free_frames()` reads the order from the frame, return some of them at whatever order they last had as the head of some other block.

**Cause**
A frame's order field is written when the frame becomes the head of a block and is not cleared when it stops being one. A frame that headed an order-3 free block, was merged into a larger one and later handed out as the fifth frame of an order-9 allocation still says 3. That is harmless while it is a tail: nothing reads the order of a tail. It stops being harmless the moment the owner frees that frame alone, because `free_frames()` now takes the field as the size of what is returned and would give the buddy eight frames for one.

Both callers that take a block apart knew this and wrote the same loop before their first single free: `set_order(0)` on every frame of the block. The rule was correct and lived in two call sites as a habit. A third caller would have had to know it too, and nothing would have told it: the frees compile and, with a fresh machine's frames, even work.

**Change**
The rule becomes a function, and the function is what a caller reaches for:

```cpp
// before, in create_contiguous() and in fill_block_locked()
for (usize index = 0; index < nr_frames; ++index) {
    block[index].set_order(0);
}

// after
split_block(block);
```

`split_block(frame, order)` turns a block of order k into `2^(k - order)` blocks, writing `order` into each new head. It takes no lock: the block is the caller's, and the only other reader of an order field, `FrameBuddy::is_mergeable()`, reads it on frames whose role says they are on a free list, which an owned frame's does not. A debug build asserts that the frame heads a block (its pfn is aligned to the block's order), that it is not on a free list, and that `order` is not above the block's.

After this change the order field has three writers in the kernel: the buddy when it frees a block, the buddy when it hands one out, and `split_block()`.

**Reference**
Linux `split_page()` in `mm/page_alloc.c`: turns a non-compound order-n page into n order-0 pages, each with its own reference count, for exactly the case of a caller that will free them one by one. Zircon has no counterpart; its `pmm_alloc_contiguous()` answers a list of single pages and never a block.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed. The same on four nodes (`NUMA=1`).

`ktest all` on dbg, default and four-node: 62 ran, 60 passed, 2 skipped. The count is one above the last step's because of the new test, which allocates a block of order 3, splits it into four of order 1, splits the first of those into two frames, checks the heads' orders at each stage, and frees the five parts alone. The contiguous VMO and large-page tests, which run both changed callers, are among the 60. `format_cpp.py` left the changed files as they were; clang-tidy reports nothing about the new function or its calls.

Not checked: that the parts coalesce back into one block of order 3. Another allocation can take one of them between the frees and the look.

**Deliberately not done**
- `PageFrame::set_order()` stays public. With `split_block()` in place nothing outside the buddy needs it, and restricting it is a small change of its own.
- `alloc_frames_contiguous()`. `create_contiguous()` still takes a whole block, zeroes it and returns the surplus (`docs/repairs/261031/mm.md` item 1).
- Tail stamps (`compound_offset`) are not touched by a split. Only the object cache stamps tails, and it never splits.

**How it was found**
Reading the two callers while checking that `free_frames()` could lose its order parameter.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a tail frame of an allocated block carry a non-zero order, and when does that start to matter?
- Why does `split_block()` need no lock, although the buddy reads order fields under the zone lock?
- What made the hand-written loop a weak guarantee, when both callers had it right?

## A contiguous run is the allocator's to make: alloc_frames_contiguous() (2026-11-12)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `alloc_frames_contiguous(flags, n, align_log2, home)`
- `kernel/mm/vm_object_paged.cpp` — `create_contiguous()` calls it and no longer takes a block apart itself
- `kernel/tests/pm_node_test.cpp` — `PmNode.ContiguousFramesAreAdjacentAlignedAndFreedAlone`
- `docs/repairs/261031/mm.md` — item 1 says where its code now is

**Symptom**
None. `create_contiguous()` allocates what it did, the same way.

**Cause**
"`n` adjacent frames at an alignment" is a request about physical memory, and the VMO was answering it with what it knew of the buddy's insides: that a block of order k is aligned to `2^k` pages, so the order must be the larger of what the count and the alignment need; that the block's descriptors are consecutive, so the frames can be reached by indexing; that the frames past `n` have to be given back; and that each frame needs an order of its own before it is freed alone. Four facts about the allocator, held by a caller.

The cost is not that the VMO had them wrong. It had them right. It is that the next caller with the same need, a driver's buffer or a table for a device, would have to hold the same four facts, and that the allocator could not change any of them (serve a run from two adjacent smaller blocks, return the tail in larger pieces) without finding and changing its callers.

**Change**
The request has a function, in the caller's terms:

```cpp
// before, in VmObjectPaged::create_contiguous()
auto const allocated = alloc_frames(alloc_flags | Gaf::Zero, order);
auto *const block = *allocated;
split_block(block);
for (usize index = nr_pages; index < nr_frames; ++index) {
    free_frames(&block[index]);
}
// ... then block[index] for each page kept

// after
auto frames = alloc_frames_contiguous(alloc_flags | Gaf::Zero, nr_pages, alignment_log2);
// ... then the frames of the list, in address order
```

The body is the VMO's, moved: compute the order, take one block, `split_block()` it, put the first `n` frames on a list, free the rest one at a time. It answers a list and not a pointer to the first frame, so that a caller never indexes from one descriptor to the next; that the descriptors happen to be consecutive is the buddy's fact again.

`create_contiguous()` keeps its own check that the order fits, because it makes that check before it creates the object the frames go into, and answers `NoMemory` for it as before.

**Reference**
Zircon `pmm_alloc_contiguous(count, alloc_flags, align_log2, pa, list)` in `kernel/vm/pmm.cc`: a count, an alignment, and a list of single pages back. Linux `alloc_contig_pages(nr_pages, gfp, nid, nodemask)`, whose callers free with `free_contig_range()`.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed. The same on four nodes (`NUMA=1`).

`ktest all` on dbg, default and four-node: 63 ran, 61 passed, 2 skipped; one more than the last step, the new test. It asks for three frames at a four-page alignment and checks that the list holds three frames of order 0, each one page after the last, the first on a 16 KiB boundary; then that a run of no frames and an alignment no block has are both `InvArgs`. `VmoContiguous.CreatesContiguousMemory`, which runs the changed caller, passes. `format_cpp.py` is clean; clang-tidy reports nothing in `pmm.cpp` or `vm_object_paged.cpp`.

**Deliberately not done**
The two costs `docs/repairs/261031/mm.md` item 1 records: the whole block is zeroed though only `n` frames are kept, and the tail is returned frame by frame, each with its own trip through the zone lock. They are performance items, to be measured and decided with the user; this change only puts them in one function. Nor does a run wait at the gate: `Gaf::CanWait` is asserted off, as for the other list forms.

**How it was found**
None. The contiguous shape of `.agents/tasks/pmm-redesign/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which four facts about the buddy did `create_contiguous()` have to know, and which of them does it still?
- Why does the function answer a list when the frames are consecutive descriptors a caller could index?
- The move changed no cost. What did it change about fixing the costs later?
