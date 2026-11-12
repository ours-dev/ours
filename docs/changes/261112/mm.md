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
