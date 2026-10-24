# mm changes

## A fault that widens a present entry reads it again under the object's lock (2026-10-24)

**Files**
- `kernel/mm/vm_mapping.cpp`, `kernel/mm/include/ours/mm/vm_mapping.hpp` — `widen_present_locked()` reads the entry again once it holds the object's lock, and loses its `existing_mmuf` parameter
- `kernel/commands/vmo_snapshot_test.cpp` — `check_widen_reads_entry_again()`
- `.agents/tasks/user-pager/PLAN.md` — step 4 split into four commits, the user's call

**Symptom**
Latent, cannot be triggered today. An access that needs a permission a present entry lacks (an instruction fetch through an entry without Execute) can map back a frame its object no longer has, once anything takes a frame out of a mapped object. `take_pages()` (`.agents/tasks/user-pager/` step 4) is the first thing that will.

**Cause**
The objects:

- The address space's lock, which a fault holds throughout.
- The object's lock: the VMO's, which is its page tree's.
- The page table entry, and the frame F it names.

The events, in the order that goes wrong:

1. The faulting thread, holding the address space's lock, reads the entry: present, frame F, without Execute (`arch_aspace.query()` in `VmMapping::page_fault()`). It has not taken the object's lock yet.
2. Another thread holds the object's lock and takes F out of the object. Taking a frame out unmaps it from every mapping first, so it unmaps this entry. It needs no address space lock to do that, only the object's.
3. The faulting thread gets the object's lock, and the non-write branch of `widen_present_locked()` maps F again with Execute, from what it read in step 1.

The entry now names a frame that is in another object, or free. The write branch never had the problem: it asks the object for the page (`require_owned_page()`) under the lock. The non-write branch used the frame and the Write bit it had read before the lock, as if the entry could not change while the address space's lock was held. That is true of faults, which all take that lock, but not of anything done under the object's lock alone: unmapping a frame that leaves, or a clone taking Write off the pages it is about to share (`remove_write_locked()`). The second could put back a Write the clone had just taken away, if an entry could have Write and lack Execute while its region had both, which W^X rules out today.

**Change**
Read the entry again once the object's lock is held. A frame leaves an object only under that lock and only after its entries are unmapped, and Write leaves an entry only under it too, so what the entry says under the lock is current. If it is gone or names another frame, the fault answers Ok without mapping anything, as it does for a fault some other CPU resolved, and the access retries into whatever is there now: a missing entry goes the demand path and maps the object's page.

```cpp
// before
} else {
    new_mmuf = (mmuf & ~MmuFlags::Writable) | (existing_mmuf & MmuFlags::Writable);
}
// after
} else {
    PhysAddr now = 0;
    MmuFlags now_mmuf{};
    if (Status::Ok != m_aspace->arch_aspace().query(va, &now, &now_mmuf) ||
        now != existing) {
        return {Status::Ok, 0};
    }
    new_mmuf = (mmuf & ~MmuFlags::Writable) | (now_mmuf & MmuFlags::Writable);
}
```

Rejected: asking the object for its page again, through the cursor, and mapping that. It answers the same question at a higher cost, and leaves open which Write a frame it did not read from the entry should keep. Rejected too: taking the object's lock before reading the entry, which is the order Zircon's fault path has (it looks the page up first and asks the page tables after); here it would move the lock ahead of the spurious-fault shortcut, which needs no object at all.

**Reference**
Zircon `VmMapping::PageFaultLocked()` (`kernel/vm/vm_mapping.cc`) takes the object's lock first and calls `PageFaultLockedObject()`, which looks the page up through the object's cursor before it touches the entry, so the frame it maps is always the object's current one. Ours keeps its order and reads the entry a second time instead.

**Verification**
- `vmosnapshot.test`, 223 checks pass, 5 of them new. The test holds the object's lock while another thread's instruction fetch, through an entry that lacks Execute, waits for it: the thread is parked, so it has read the entry. The test unmaps the entry and lets go. The fetch answers Ok and the entry stays unmapped; the fetch retried maps the object's page, executable.
- Control: with the second read switched off, "the fetch leaves the entry unmapped instead of mapping its frame back" fails. Unmapping an absent entry succeeds (`UnmapVisitor::on_absent()`), so the old branch mapped the frame it had read back.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass. clang-tidy reports nothing on the lines changed.

**Deliberately not done**
The demand path, for an entry that is absent when the fault looks, already asks the object under its lock and maps what it gets, so it is unchanged. A physical object has no lock and no frame that leaves, so its branch is unchanged too.

**How it was found**
Reading the fault path while designing `take_pages()` (`.agents/tasks/user-pager/DESIGN.md`, "Found while designing"): the entry is read at `vm_mapping.cpp:645`, before the object's lock, and remapped from at `:769-776`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Which two locks does the fault hold, in which order, and which of them does an operation that takes a frame out of an object hold?
2. Why is reading the entry again under the object's lock enough, without asking the object for its page?
3. Why is answering Ok without mapping anything safe, and what happens on the retry?

## A user address space maps 2 MiB leaves with a table set aside, kept in the page table's descriptor (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::MmuBody`, the page-table role's body: the chain of tables set aside
- `kernel/mm/include/ours/mm/page_table_deposits.hpp`, `kernel/mm/page_table_deposits.cpp` (new), `kernel/mm/BUILD.gn` — `PageTableDeposits`, the engine's `TableDeposits` on the descriptors
- `kernel/mm/pmm.cpp` — `alloc_page_table()` clears the body (`become<MmuBody>()`); `free_page_table()` asserts no table is still set aside
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp` — `PageTable::Flags::SplitsLargeLeaves`, `with_deposits()`, the leaf size out of `query_mapping()`
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — a user address space's table splits its leaves; `query()` passes the size out
- `kernel/commands/large_page_test.cpp` (new), `kernel/commands/BUILD.gn` — `largepage.test`

**Symptom**
None: step 2 of `.agents/tasks/large-pages/`. The engine could split a leaf into a table set aside (`docs/changes/261024/arch-paging.md`), but nothing kept such tables, and every caller still used the engine without them.

**Cause**
The objects:

- A second-level table page of a user address space (x86's page directory), and its descriptor, whose `Mmu` role had no body.
- `page::MmuBody`: `deposits`, on the table that holds the leaves, names the first table set aside; `next_deposit`, on each table set aside, names the next. A chain of bare pointers through the bodies, not the descriptor's list hook, because the head lives in a body and a zeroed body cannot be an empty intrusive list.
- `PageTableDeposits`: the engine's four calls on that chain. `reserve()` takes a zeroed table from `alloc_page_table()` and pushes it; `withdraw()` pops one for a split; `discard()` pops one and frees it; `discard_all()` frees them all. No state of its own; a global count of tables set aside (`nr_held()`) for tests.
- `PageTable::Flags::SplitsLargeLeaves`: the kind of a table, given by `ArchVmAspace::init()`, set for a user address space only (D5). `with_deposits()` is the one place that picks `PageTableDeposits` or `NoDeposits` for an engine call.

The events: mapping a contiguous, aligned 2 MiB block in a user address space reserves a table and writes a leaf; an unmap or protect of part of it withdraws the table and splits the leaf into it; an unmap of the whole leaf discards its table; the teardown of the address space discards what is left; freeing a table that still has some set aside is caught by an assertion.

All of it runs under the page table's spin lock, with interrupts off, as the engine's map already did when it allocated a table there. Nothing new nests under the lock: the allocator was already called under it.

The TLB needs nothing new. The split changes no translation, only the size of the entry, and the operation that follows changes at least one page of the leaf and invalidates it (`ArchVmAspace::invalidate()`, `invlpg` per page on every CPU running the address space): `invlpg` of any address of a large page drops the whole large translation, and every paging-structure cache entry with it. Intel's rule against changing a page's size together with its frame or attributes in one write is kept: the split writes the same frames and attributes, and only the change after it, on 4 KiB entries, alters attributes.

**Change**
The descriptor body, and its use in the engine's calls:

```cpp
struct MmuBody {
    constexpr static auto ROLE = Role::Mmu;

    PageFrame *deposits;
    PageFrame *next_deposit;
};
```

```cpp
auto PageTableDeposits::reserve(PhysAddr const owner) -> bool {
    auto const table = alloc_page_table(PAGE_SIZE, PAGE_SIZE);
    if (table == 0) {
        return false;
    }
    auto &body = body_of(owner);
    auto *const frame = phys_to_frame(table);
    frame->as<page::MmuBody>().next_deposit = body.deposits;
    body.deposits = frame;
    ...
}
```

Every `PageTable` operation goes through `with_deposits()`:

```cpp
auto result = with_deposits([&](auto &deposits) {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    return Engine::unmap(m_pgd_phys, phys_to_virt, deposits, va, n);
});
```

`alloc_page_table()` used to set the role alone (`set_role(Mmu)`), which leaves whatever the body held; it now clears it with `become<MmuBody>()`, so a new table starts with no chain. A table built at boot is stamped Mmu over zeroed memmap storage, and is the kernel's, which never sets tables aside.

A LIFO chain rather than Linux's FIFO: any table set aside with an owner serves any of its leaves, so the order is immaterial, and a push and pop of a singly linked chain need no tail.

**Reference**
Linux keeps the deposited tables on `ptdesc->pmd_huge_pte`, in the descriptor of the PMD table page (`pgtable_trans_huge_deposit()`, `pgtable_trans_huge_withdraw()`, `mm/pgtable-generic.c`), chained through each table's `lru`; ours chains through the bodies for the reason above. The invalidation argument is Intel's: `invlpg` invalidates the TLB entries for the page containing the address, whatever its size, and all paging-structure cache entries for the current PCID.

**Verification**
- `largepage.test`, dbg image, 8 checks pass: a contiguous 2 MiB block maps as one leaf (the query says 2 MiB) with one table set aside; an unmap of one page splits it, the page is gone and its neighbours are 4 KiB entries to the same frames, with the table no longer set aside; a protect of the first page takes Write from it alone; the split range mapped again stays small entries, since unmapping never frees the table it was split into; a leaf mapped in the next 2 MiB and unmapped whole gives its table back; with the address space gone, the count of tables set aside is what it was.
- Control: with the user address space's table made without `SplitsLargeLeaves`, 5 of the 8 fail (the leaf takes no table, and a part of it is InvArgs again). Restored, all pass.
- `vmosnapshot.test`, `pagesource.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not exercised: a table set aside freed at teardown with its leaf still mapped (`discard_all()`), which the host test covers; in the kernel it would also log that pages were still mapped.

**Deliberately not done**
Nothing maps a user leaf yet but this test: the VMO side and the mappings are steps 3 and 4. A split range is never put back together into a leaf (D8).

**How it was found**
None: step 2 of `.agents/tasks/large-pages/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why does the split need no TLB invalidation of its own?
2. Why are the tables set aside chained through the descriptor bodies rather than through the descriptor's list hook?
3. Why does a range that was split stay small entries when it is mapped again, and what would it take to change that?

## A VMO with LargePages fills a hole with the 2 MiB block it lies in (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/vm_object.hpp` — `VmoFlags::LargePages`, kernel-internal (D2)
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `VmCowPages::Flags::LargeBlocks`, `create()` with flags, `fill_block_locked()`, and `require_owned_page()` asking it before one page
- `kernel/mm/vm_object_paged.cpp` — `create()` passes LargePages on as LargeBlocks
- `kernel/commands/large_page_test.cpp` — five checks of blocks
- `.agents/tasks/large-pages/DESIGN.md`, `PLAN.md` — nothing records a block, revised with the user

**Symptom**
None: step 3 of `.agents/tasks/large-pages/`. A VMO's pages were allocated one at a time, so no range of one was ever physically contiguous and 2 MiB aligned, and no mapping of it could use a 2 MiB leaf.

**Cause**
The objects:

- A node that takes blocks: a VmCowPages with LargeBlocks, no parent and no page source, made by `VmObjectPaged::create()` for a VMO created with LargePages.
- A block: an order-9 buddy allocation, 512 physically contiguous frames on a 2 MiB boundary.
- A frame's order: kept in its descriptor by the buddy. The head of an allocation says the order it was allocated at, and `free_frames()` of a list frees each frame by its own order.

The events, on a fault or a commit:

1. `require_owned_page()` finds nothing at the offset (a read of a node with no relatives comes here too).
2. `fill_block_locked()`: the node takes blocks, the block's 2 MiB range fits in the node, and the range holds no page and no zero marker (`has_content_in()`, one search of the page map).
3. An order-9 block is allocated, zeroed. Every one of its 512 frames is made an order-0 frame and a Vmm page, and put in its slot.
4. The faulting offset gets its page of the block; the other 511 are there for the faults after it.
5. Any condition missing, the allocator out of blocks, or the page map unable to grow: nothing changes, and the hole gets one page as before.

Nothing records that the 512 pages were one block, which is the revision of the design made at this step. The head would say order 9 if left alone, and a free by the frame's own order would then free 2 MiB from under 511 pages still in use; and any record would have to be cleared by every path that takes one page away (a copy-on-write migration, a merge, `take_pages()`). Whether a range is still one block is a question for the pages, asked by their addresses when a mapping wants a 2 MiB leaf (step 4). The question is needed anyway: a clone's view of a range can mix pages of its own with an ancestor's block.

**Change**

```cpp
auto *const block = alloc_frames(m_gaf | Gaf::Zero, ORDER);
if (block == nullptr) {
    return nullptr;
}
for (usize i = 0; i < NR_PAGES; ++i) {
    block[i].set_order(0);
    block[i].become<page::VmmBody>();
}
for (usize i = 0; i < NR_PAGES; ++i) {
    if (Status::Ok != m_pages.insert_page(first + i, &block[i], VmPageMap::Tag::Owned)) {
        // every page put in is taken out, every frame freed alone
        ...
        return nullptr;
    }
}
return &block[(offset - start) >> PAGE_SHIFT];
```

`block[i]` is safe pointer arithmetic: the descriptors of one block are contiguous, since a block never crosses a section of the memmap (`FrameBuddy`, `SparseMemMap`'s `static_assert`).

In `require_owned_page()`, before one page is allocated:

```cpp
if (!found.zero_marker) {
    m_target->lock().assert_held();
    if (auto *const page = m_target->fill_block_locked(m_offset)) {
        advance();
        return ktl::ok(RequireResult{.page = page, .writable = true});
    }
}
```

**Reference**
Linux `split_page()` (`mm/page_alloc.c`): a non-compound high-order allocation made into its sub-pages, each freed alone. Linux's transparent huge pages do otherwise: the 2 MiB page stays a compound folio whose head knows its order, and `split_huge_page()` takes it apart later. Ours does what `split_page()` does at once, and leaves the question of contiguity to the mapping, for the reasons above.

**Verification**
- `largepage.test`, dbg image, 13 checks pass, 5 of them new: a write to page 5 of a node that takes blocks fills its block, 512 pages, 2 MiB aligned, contiguous, each of order 0, and page 5 is the page the write got; a read of the second 2 MiB fills a block of its own; a hole in the tail, where no block fits, gets one page; a hole in a range that holds a zero marker gets one page; a node that does not take blocks gets one page.
- Control: with `fill_block_locked()` refusing every hole, 3 of the 13 fail; with the frames left as the buddy gave them, the head says order 9 and 2 fail. Restored, all pass.
- `vmosnapshot.test` (223), `pagesource.test` (48), `usercopy.test`, `vmaseal.test` pass. clang-tidy reports nothing on the lines changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not tested: the fallback when the allocator has no 2 MiB block, which cannot be forced from a test.

**Deliberately not done**
- Mapping a block with a 2 MiB leaf: step 4.
- Blocks in a clone, a node with a page source, or a hidden node: only a VMO's own root takes blocks (D2).
- Freeing a block whole when its node dies: each page is freed alone, as every page is; the buddy merges them back.

**How it was found**
None: step 3 of `.agents/tasks/large-pages/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. What would go wrong, and where, if the head of a block kept the order 9 the buddy gave it?
2. Why is the block filled only when its whole range holds nothing, not even a zero marker?
3. Why does nothing record that the 512 pages were one block, and who answers that question instead?

## A fault or a populate maps a whole block of a VMO as one 2 MiB leaf (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `FoundBlock`, `find_block_locked()`, `Cursor::skip()`
- `kernel/mm/include/ours/mm/vm_object_paged.hpp` — `VmObjectPaged::find_block_locked()`, the object's offset and size applied
- `kernel/mm/include/ours/mm/vm_mapping.hpp`, `kernel/mm/vm_mapping.cpp` — `map_block_locked()`, called by the fault path and by `map_paged()`; `MappingCoalescer::skip_to()`; `map_paged()` annotated with the lock it runs under
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `Paging::second_level_empty()`
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp`, `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — the same question through `PageTable` and `ArchVmAspace::large_leaf_slot_empty()`
- `kernel/commands/large_page_test.cpp` — ten checks of mappings
- `.agents/tasks/large-pages/DESIGN.md` — D7 revisited and kept

**Symptom**
None: step 4 of `.agents/tasks/large-pages/`. A VMO could hold 2 MiB blocks (step 3) and the page table could hold and split 2 MiB leaves (steps 1 and 2), but every mapping still put each page in its own 4 KiB entry.

**Cause**
A 2 MiB leaf has one physical start and one set of permissions for 512 pages, so writing one is a claim about all of them, and the claim has to be checked against what is true when it is written. The objects:

- The mapping: its range, its protection ranges (one set of permissions per region), its object and the object's offset.
- The page table's entry for the 2 MiB window: empty, a leaf, or a pointer to a table of small entries.
- The 512 pages the object shows in the window, each found by the walk of its node and its ancestors (`find_content()`), each with the node that holds it.

The events of a fault, or of a populate reaching a 2 MiB boundary:

1. The page asked for is found or filled as before; in a node that takes blocks, filling it fills its block (step 3).
2. `map_block_locked()` asks, cheapest first: a user address space, since only its page table splits leaves (D5); the window inside the mapping, with one protection over all of it, the one the page has; the window's entry empty, neither a leaf nor a table; and the 512 pages one block (`find_block_locked()`).
3. All four hold: one leaf maps the window, with Write only if the block's node is the mapping's own node and not hidden. The 511 faults after this one do not happen.
4. Any fails: the page gets its 4 KiB entry, as before.

`find_block_locked()` asks the pages, because nothing else knows: the first page must be a 2 MiB aligned frame, and each of the others the frame after the one before it, held by the same node. The first and the last are asked before the rest. The node matters as much as the frames. A clone that takes the first page of a shared block for itself (it was the page's last viewer, so the hidden node gave it the frame rather than a copy) sees a window that is still physically contiguous, but its first page is its own and the other 511 are still shared with the origin. A leaf would be writable, from its first page, and the clone's writes to the other 511 would reach the origin.

The window's entry must be empty, and not merely without a leaf: a window that was split keeps its table (unmapping never frees one), and a leaf written over it would lose every small entry in it; the engine's map refuses instead, and the fault would fail.

What a leaf meets afterwards is what steps 1 and 2 already handle: a protect, an unmap, or a clone's remove-write of part of it splits it into its deposited table; a write to a read-only leaf over a shared block splits it and copies or takes only the page written (D7). The user revisited D7 when it was made: one write leaves the other 511 pages of the writer's window small for good, which they found unfair. Kept as it is, which is Linux's choice since 2020; putting such windows back into leaves (D8) is to be weighed against the measurement of step 6.

**Change**

```cpp
auto VmMapping::map_block_locked(VmObjectPaged &vmo, VirtAddr const window, MmuFlags const mmuf)
    -> ustl::Option<ustl::Pair<Status, usize>> {
    if (!m_aspace->is_user()) {
        return ustl::NONE;
    }
    // the window inside the mapping, one protection over it, its entry empty
    ...
    auto const block = vmo.find_block_locked(m_vmo_ofs + (window - m_base));
    if (!block) {
        return ustl::NONE;
    }
    auto leaf_mmuf = mmuf;
    if (!block->writable) {
        leaf_mmuf &= ~MmuFlags::Writable;
    }
    ...
    auto const status = arch_aspace.map(window, block->base, LARGE_LEAF / PAGE_SIZE, leaf_mmuf,
                                        MapOptions::ErrorIfExisting, &nr_mapped);
    return ustl::some(ustl::Pair<Status, usize>{status, nr_mapped});
}
```

In the fault path, after the page is found:

```cpp
if (auto const mapped = map_block_locked(*paged, ustl::mm::align_down(va, LARGE_LEAF), mmuf)) {
    return *mapped;
}
```

In `map_paged()`, at a 2 MiB boundary with the whole window in the region, the batch so far is committed, the block is mapped, and the coalescer and the cursor step past the window (`skip_to()`, `skip()`).

The leaf is tried only in a user address space. The kernel's page table has no deposits, and a kernel object can be physically contiguous (the boot image's pages, wired), so without the check a kernel mapping could get a leaf that could never be changed in part.

**Reference**
Linux maps a PMD only when the PMD is empty (`do_huge_pmd_anonymous_page()` is reached from `__handle_mm_fault()` only when `pmd_none()`), only when the VMA covers the aligned range (`thp_vma_suitable_order()`), and with the folio it allocated; ours has no folio and asks the pages instead (step 3). Linux's write fault on a shared huge PMD splits it and falls back to a 4 KiB copy (`do_huge_pmd_wp_page()`), as ours does, since `3917c80280c9` ("thp: change CoW semantics for anon-THP", 2020), which gave up copying the whole 2 MiB on the first write because "we don't necessary want to unshare full PMD range from the parent on the first access", naming Redis. Zircon maps large pages only for physically contiguous objects.

**Verification**
- `largepage.test`, dbg image, 23 checks pass, 10 of them new: a write fault in a large-page object maps its 2 MiB as one writable leaf; with one page of the window read-only, a fault maps only its own page; a protect of one page of the leaf splits it, and only that page loses Write; an object offset 4 KiB off a 2 MiB boundary gets small entries; a populate mapping maps each block as one leaf; a fault in a window split before maps its own page only; a read of a clone maps the shared block as one read-only leaf; a write to one page of it splits the leaf, the page a writable copy and its neighbour still the block, read-only; a window whose first page the clone took for itself, the rest still shared, is no block.
- Control runs, each with one condition taken out: no leaf at all fails 5 checks; `find_block_locked()` without the check of the holding node fails exactly the check of the window the clone half took; without the check of the empty entry, the fault in the split window fails. Restored, all pass.
- `vmosnapshot.test` (223), `pagesource.test` (48), `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test`, `x86-paging-smoke` pass. clang-tidy reports nothing on the lines changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass. Every program the boot starts is mapped through these paths.
- Performance: not measured; step 6.

**Deliberately not done**
- Placing a large-page object's mapping at a 2 MiB congruent address when the kernel picks it: step 5. The tests place theirs.
- Putting a split window back into a leaf (D8).
- A leaf for a VMO with a page source: its pages come one supply at a time and nothing makes them a block; nothing stops a leaf if they happen to be one.

**How it was found**
None: step 4 of `.agents/tasks/large-pages/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. A clone's window is physically contiguous and 2 MiB aligned. Why may it still not be mapped as one leaf?
2. Why must the window's entry be empty, and not merely free of a leaf?
3. Why is a leaf never tried in the kernel's address space, even for a physically contiguous object?

## A large-page object's mapping placed by the VMM starts where its offset falls in a block (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/vm_area.hpp`, `kernel/mm/vm_area.cpp` — `create_subaom_preamble()` takes a block phase; `create_mapping()` passes one for a LargePages object
- `kernel/mm/include/ours/mm/vm_object.hpp` — `VmObject::flags()`
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_mapping.cpp` — the block's 2 MiB is one constant, `VmCowPages::BLOCK_SIZE`, where it was four
- `kernel/commands/large_page_test.cpp` — two checks of placement

**Symptom**
None: step 5 of `.agents/tasks/large-pages/` (D6). A mapping the VMM places went at the lowest free page-aligned address. For a large-page object that is 2 MiB aligned only by luck, and when it is not, no window of the mapping lines up with a block of the object, so step 4 never maps a leaf there.

**Cause**
A leaf maps a 2 MiB aligned window of addresses, and a block is a 2 MiB aligned range of object offsets (step 3). Window and block coincide only when address and object offset agree modulo 2 MiB. The objects:

- The area's search for a free spot (`alloc_spot()`), which already takes an alignment and was always given a page.
- The block phase: the mapping's object offset modulo 2 MiB.

The events, when a mapping of a LargePages object is placed by the VMM:

1. `create_mapping()` computes the phase from the object offset.
2. `create_subaom_preamble()` asks for a spot of the mapping's size plus the phase, aligned to 2 MiB, and places the mapping at the spot plus the phase. Every address of it then agrees with its object offset modulo 2 MiB, and each whole block of the object inside it falls on a window. Only addresses are spent on it, never memory.
3. With no such spot, or a mapping shorter than a block, it is placed as before, and is mapped with small entries only.

A mapping placed by its creator is not moved: the address is the creator's.

**Change**

```cpp
constexpr usize BLOCK = VmCowPages::BLOCK_SIZE;
if (block_phase && size_aligned >= BLOCK && size_aligned <= ~usize{0} - *block_phase) {
    auto const padded = size_aligned + *block_phase;
    if (auto const spot = alloc_spot(padded, BLOCK, m_base, m_base + m_size - 1)) {
        return ktl::ok(ustl::make_pair(*spot + *block_phase, size_aligned));
    }
}
```

The pad is the phase, not a whole block: the mapping at `spot + phase` ends at `spot + phase + size`, which is where the padded spot ends.

The block's 2 MiB was written out in four places by steps 3 and 4 (`fill_block_locked()`, `find_block_locked()`, `VmObjectPaged::find_block_locked()`, the mapping's leaf size), and this step needed a fifth; it is now `VmCowPages::BLOCK_SIZE`, which the others name.

**Reference**
Linux `thp_get_unmapped_area()` and `__thp_get_unmapped_area()` (`mm/huge_memory.c`): the length is padded by `PMD_SIZE`, the unmapped area searched for, and the result moved up so that the address and the file offset agree modulo `PMD_SIZE`. Linux pads by a whole `PMD_SIZE` because its search takes no alignment; ours searches with one, so the phase is enough. Linux also leaves alone a range that holds no whole aligned PMD; ours pads any mapping at least a block long, which costs at most the phase in addresses.

**Verification**
- `largepage.test`, dbg image, 25 checks pass, 2 of them new. After a page the VMM placed first, so that the lowest free address is 4 KiB past a boundary, a mapping at object offset 0 starts on a block boundary and its fault maps a leaf; a mapping at object offset 4 KiB starts 4 KiB past a boundary, and its window at object offset 2 MiB maps a leaf.
- Control: with no phase passed, the check at offset 0 fails. Without the spacer page the check at offset 0 had passed by luck, which is why the spacer is there; the check at offset 4 KiB lands right by luck in that run instead, so each check alone is not proof, the pair is.
- `vmosnapshot.test` (223), `pagesource.test` (48), `vmaseal.test`, `vmaundo.test`, `vmawx.test`, `usercopy.test` pass. clang-tidy reports nothing on the lines changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
Placing other objects so: only a LargePages object fills holes with blocks, so only its mappings gain from it. Offering the flag to programs through `vmo_create`: open until the measurement (D2).

**How it was found**
None: step 5 of `.agents/tasks/large-pages/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why does a mapping at object offset 4 KiB start 4 KiB past a 2 MiB boundary rather than on one?
2. Why is the spot padded by the phase and not by a whole 2 MiB?
3. What does the placement cost, and what does it not cost?
