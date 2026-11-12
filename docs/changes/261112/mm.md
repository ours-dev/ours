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
