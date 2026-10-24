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

## Title: the walk over every mapping of a node's pages unmaps as well as taking Write (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/types.hpp` — new `RangeChange { RemoveWrite, Unmap }`
- `kernel/mm/vm_cow_pages.cpp` and header — `remove_write_locked()` becomes `change_range_locked(offset, len, change)`; the clone passes `RemoveWrite`
- `kernel/mm/vm_object_paged.cpp` and header — `remove_write_of_cow_range_locked()` becomes `change_cow_range_locked()`
- `kernel/mm/vm_object.cpp` and header — `remove_write_locked()` becomes `change_range_locked()`
- `kernel/mm/vm_mapping.cpp` and header — `remove_write_locked_object()` becomes `change_range_locked_object()`, which unmaps the mapping's part of the range on `Unmap`, and stops the kernel if that fails
- `kernel/commands/vmo_snapshot_test.cpp` — the attachment checks unmap through the node too (3 new checks)
- `kernel/commands/large_page_test.cpp` — an unmap through the node splits a leaf (2 new checks)

**Symptom**
None, a step of `.agents/tasks/user-pager/` (4.2). `take_pages()` (4.3) takes frames out of a node while they may be mapped, in any number of address spaces, through the node's own object and every slice of it. Before a frame leaves, every entry naming it has to go; an entry left behind maps a frame the node has handed to someone else, and the program reads or writes another's memory through it.

**Cause**
The node already had the walk that reaches every mapping of its pages: a clone takes Write away from them through `VmCowPages` → each `Attachment` (the object and every slice over the node) → each `VmMapping` of that object, each translating the node's offsets into its own. It could only take Write away. Unmapping needs exactly the same walk with a different last step, so the walk takes the step as an argument instead of being written a second time: two copies of the offset clamping and the attachment loop would drift the first time one is fixed.

The unmap has no failure path to offer. A frame half unmapped cannot be put back into the mappings that lost it without the caller undoing its own operation, and every caller is about to give the frame away. So it must not fail, and the argument that it cannot is the reason this step waited for `.agents/tasks/large-pages/`: an x86 unmap fails only on part of a 2 MiB leaf that has nothing to split it into, and since large-pages every leaf of an object's mapping is in a user address space that set a table aside for it when the leaf was mapped. Before that, a partial unmap of a leaf was InvArgs, and the walk would have had to fail or unmap more than it was asked.

**Change**
The last step of the walk, before and after:

```cpp
// before: only Write could come off
auto VmMapping::remove_write_locked_object(u64 vmo_offset, u64 len) -> void;

// after
auto VmMapping::change_range_locked_object(u64 vmo_offset, u64 len, RangeChange change) -> void {
    ...
    if (change == RangeChange::Unmap) {
        DEBUG_ASSERT(m_aspace->is_user(), "unmapping pages under a kernel mapping of them");
        auto const status =
            arch_aspace.unmap(base, size >> PAGE_SHIFT, UnmapOptions::None, nullptr);
        ASSERT(Status::Ok == status, "could not unmap pages leaving their object");
        return;
    }
    // RemoveWrite: as before, region by region, only where the region allows Write
```

Only the page tables change; the mapping's record of what it allows (`m_protection_ranges`) stays, so the next access faults and maps whatever the node holds then. The whole range is unmapped in one call, not region by region: a region with no permission has no entries, and unmapping an absent entry succeeds.

Locking is the clone's: the node's lock is held across the walk, the mapping reads its record under that lock (it is written only under it), and the page table takes its own. A fault that read the entry before the walk re-reads it under the node's lock before it widens it (4.1), so it cannot put back an entry the walk removed.

Rejected: a second, parallel set of four functions for unmapping. Same walk, same clamping, twice.

**Reference**
Zircon `VmCowPages::RangeChangeUpdateLocked()` → `VmObjectPaged::RangeChangeUpdateLocked()` (clamps to the object's `cow_range_`, and passes the change on to its references) → `VmObject::RangeChangeUpdateMappingsLocked()` → `VmMapping::AspaceUnmapLockedObject()`, which `ASSERT`s the unmap succeeded and `DEBUG_ASSERT`s the mapping is not a kernel one, or `AspaceRemoveWriteLockedObject()`, chosen by `RangeChangeOp` (`Unmap`, `RemoveWrite`, and kinds this kernel does not have yet: `UnmapZeroPage`, `UnmapAndHarvest`, `DebugUnpin`). Zircon reaches a slice through its own object list and children; here a slice is an `Attachment` of the node. Zircon also skips a node's pinned pages in this walk (`RangeChangeUpdateMappingsLocked()` on a node with `pinned_page_count_`); here pinning is the object's, and the operation refuses a pinned object before it walks. Zircon makes the unmap infallible with `EnlargeArchUnmap()`, unmapping the whole large page on a failed split; here a table set aside for the leaf makes the split itself infallible (large-pages D1).

**Verification**
- `vmosnapshot.test`, 226 checks, 3 new: an unmap through the node of the pages' third page takes it out of the object's mapping (its page 2) and the slice's (its page 1), and nothing else; a read maps it again, since the mapping still allows it; the whole range unmaps every mapping of every object over the pages.
- `largepage.test`, 27 checks, 2 new: an unmap through the node of one page of a 2 MiB leaf splits the leaf into the table set aside for it (one fewer held), the page is gone and its neighbour is a 4 KiB entry to the same frame; a read maps the page again, small, to the frame the object still holds.
- Control runs: with the Unmap step doing nothing, the 4 checks that look for a page gone fail; with the walk stopping after the first object over the node, exactly the 2 that need the slice reached fail (`largepage.test`, with one object, passes). Restored, all pass.
- `pagesource.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not exercised: the walk from an operation, since `take_pages()` is the next step; the tests call it on the node directly.

**Deliberately not done**
- `take_pages()` itself, the splice list and `supply_pages()` taking it: 4.3.
- Reaching nodes below this one in a copy-on-write tree. None needs it: only a hidden node has children (a clone moves the cloned node's pages into a new hidden parent and hangs the node under it), and `take_pages()` works on a node that is not hidden, whose pages nobody else sees. A page such a node sees through a hidden parent is shared, and 4.4 copies it before taking it.
- Zircon's other `RangeChangeOp` kinds: nothing here needs them yet.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why can the unmap walk have no failure path, and what made an x86 unmap infallible for every mapping it reaches?
- Why does the walk go through the node's attachments rather than through one back pointer to its object?
- After the walk, why does an access to the page fault and get mapped again rather than be refused?

## Title: VmMapping::map() takes mm's own MapOptions, and a populate maps only the pages that are there (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/vm_mapping.hpp` — new `MapOptions { None, Commit }`; `map(base, size, MapOptions)`, `map_paged(vmo, base, size, MapOptions)`, `map_physical(vmo, base, size)`
- `kernel/mm/vm_mapping.cpp` — `map_paged()` maps what the object has without Commit and makes every page the object's own with it; the coalescer maps with `ArchMapOptions::ErrorIfExisting` and takes no options
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `vm_cow_pages.cpp` — `Cursor::maybe_page()`
- `kernel/mm/include/ours/mm/types.hpp`, `vmas_flags.hpp` — the page table's options are `ArchMapOptions` in mm
- `kernel/mm/stack.cpp` — a kernel stack maps with Commit; `kernel/mm/vm_area.cpp` — a Populate maps with None
- `kernel/arch/x86/mm/page_table.cpp`, `arch_vm_aspace.cpp` and their headers, `kernel/arch/x86/kernel/bootstrap.cpp`, `kernel/mm/cpu_local.cpp` — `ArchMapOptions` by its new name
- `kernel/commands/vmo_snapshot_test.cpp`, `large_page_test.cpp` — objects a Populate is expected to fill are made committed; 3 new checks
- `kernel/commands/page_source_test.cpp` — a Populate of a half-supplied sourced object (6 new checks)
- `docs/repairs/261023/mm.md` — item 2 removed, the rest renumbered; `docs/repairs/261024/mm.md`, `docs/repairs/261024/arch-paging.md` — new, found on the way

**Symptom**
`docs/repairs/261023/mm.md` item 2: a Populate mapping of a VMO with a page source, with any page missing, failed with ShouldWait and the mapping was undone. Behind it, the user's question at `map_paged(..., bool commit, MapOptions options)`: why is `commit` not in `MapOptions`? Because `MapOptions` was the page table's own enum, re-exported into mm, and `commit` is a decision about the object. And both were dead: `commit` was `(void)`ed with a FIXME, and the coalescer stored the caller's `MapOptions` and mapped with a hard-coded `TryLargePage` instead, which the page table ignores as well (`(void)options` in `PageTable::map_pages()`).

**Cause**
Two layers' choices shared one name. The page table's options say what to do about an entry already there and whether to try a large page; a caller of `VmMapping::map()` has no business choosing either: an entry already there is an error for a range nobody has mapped yet, and since large-pages whether a block goes in as a leaf is the mapping's decision (`map_block_locked()`). What a caller does choose, what to do about a page the object does not have, had no type at all, only a bool nobody read.

And with no choice wired up, `map_paged()` asked the cursor for every page the way a read fault does (`require_page(false, ...)`). For an anonymous root that commits a page for every hole (the read of a hole allocates, `docs/repairs/261023/mm.md` item 3); for a node with a source it raises a request and answers ShouldWait, under the address space's lock, where nothing can wait. A populate cannot wait for a pager, so it must not ask one.

**Change**
mm's own options, at the mapping's level (user's call, 2026-10-24: "define mm's MapOptions, distinct from the arch layer's"):

```cpp
enum class MapOptions : u8 {
    None = 0,       // the pages the object has; the rest left to fault
    Commit = 0_bit, // every page made the object's own first
};
using ArchMapOptions = ::arch::paging::MapOptions;  // only mm's own code uses it
```

`map_paged()`, per page:

```cpp
if (commit) {
    auto owned = cursor->require_owned_page(1, &page_request);  // a hole gets a page, a shared one a copy
    ...
} else {
    result = cursor->maybe_page();  // own page writable, ancestor's read-only, else nothing
}
...
if (result.is_none()) {         // nothing there: the batch goes in, the next starts after it
    coalescer.commit(); coalescer.skip_to(va + PAGE_SIZE); continue;
}
```

`Cursor::maybe_page()` is `require_read_page()`'s first answer only: the page `find_content()` finds, writable when it is the node's own. A hole, a zero marker, a page a source has not supplied: nothing, and nothing allocated or asked for.

A kernel stack maps with Commit; a Populate maps with None, which is what `VmMapOption::Populate`'s comment already said it did ("maps what the object has, and does not ask the object for pages of its own"). The kernel's other populated mappings are Pinned objects committed when they were made, or physical ones, so nothing they map changes.

The behaviour that changed: a Populate mapping of a lazy object no longer commits its holes, and no longer maps the shared zero page for a clone's hole; both fault when touched. Nine test objects that a Populate was expected to fill are now made committed (`COMMITTED`, no Lazy), as their own comments ("Committed, so the page tables hold entries to change") said they were.

Rejected:
- Putting Commit into the page table's enum: the page table cannot act on it, and the arch layer would carry a bit about objects.
- No options at all on `map()`: the user chose a type of mm's own, and Commit is a real choice with a caller.
- `SkipExisting` now (Zircon's `ignore_existing`): no caller; it belongs with a range map over a mapping that may be partly mapped already.

**Reference**
Zircon `VmMapping::MapRange(offset, len, bool commit, bool ignore_existing)`: with `commit`, `LookupCursor::RequireOwnedPage()` per page (only kernel VMOs, which never have a source, so ShouldWait is asserted against); without, `LookupCursor::MaybePage()`, skipping absent pages with `SkipMissingPages()`; `ignore_existing` picks `ArchVmAspace::ExistingEntryAction::Skip` or `Error` for the coalescer. `ZX_VM_MAP_RANGE` maps without commit. Zircon's `MaybePage(will_write)` maps no ancestor page into a writable range at all; this kernel maps it read-only, as `map_paged()` did before.

**Verification**
- `pagesource.test`, 54 checks, 6 new: page 1 of a sourced object supplied and page 0 not, a Populate mapping of it is made, the supplied page is mapped and the other is not, and the provider is asked nothing; a read of the page left out asks for it, and reads the supplied bytes once it is supplied.
- `vmosnapshot.test`, 229 checks, 3 new: a Populate of a lazy object with page 1 written maps page 1 writable, leaves pages 0 and 2 unmapped, and commits nothing; `map()` with Commit of a clone of two committed pages maps both writable, on copies, the clone holding 2 pages; the address space is destroyed. One check changed: a populated clone's hole and a page past its end are left unmapped, where they were the zero page.
- `largepage.test`, 27 checks: the populate leaf check uses an object made with its blocks.
- Control: with the None path asking the cursor the old way (`require_page(false, ...)`), the Populate mapping of the sourced object is not made (the repair item's symptom), the lazy Populate commits, and the clone's hole is mapped: 3 checks fail. Restored, all pass.
- `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass: every program the boot starts still starts.

**Deliberately not done**
- The page table ignoring its options: `docs/repairs/261024/arch-paging.md` item 1, the arch layer's to fix.
- `map_physical()` mapping a second protection region from the first one's physical address, noticed while taking its options away: `docs/repairs/261024/mm.md` item 1.
- A populate racing a fault on the same page: a fault between the mapping going live and `map()` leaves an entry `map()` then fails on. Not new, since the page table always failed on an existing entry; Zircon's `ignore_existing` is the answer, when a caller needs it.
- A run of absent pages skipped in one step (Zircon's `SkipMissingPages()`): each absent page costs one lookup; a performance item, not measured.

**How it was found**
The user asked, reading `map_paged(..., bool commit, MapOptions options)`, why `commit` was not one of the options. Following both parameters down showed neither reached anything: `(void)commit`, a coalescer that stored its options and mapped with a constant, and a page table that ignores whatever it is given.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why can a populate not ask a page source for a missing page, when a fault can?
- What does Commit do to a clone's shared page, and why is that wrong for a Populate but right for a kernel stack?
- Why does a caller of `VmMapping::map()` not get to choose `TryLargePage`?

## Title: a protection-range walk's result says it is the end by being empty, with no flag beside it (2026-10-24)

**Files**
- `kernel/mm/include/ours/mm/protection_ranges.hpp` — `Enumerator::NextResult` loses `valid`; `operator bool()` is `size != 0`
- `kernel/mm/vm_mapping.cpp` — the five `while (auto [...] = enumerator.next())` loops decompose three fields; the two single lookups test the result itself
- `kernel/mm/tests/protection_ranges_smoke.cpp` — the same in its loop

**Symptom**
None, a cleanup the user asked for, reading `while (auto [valid, region_base, region_size, mmuf] = enumerator.next())` in `map_paged()`: `valid` is bound and never used, since the loop's condition already reads the result through `operator bool()`.

**Cause**
The result carried "is there a region" twice: in `valid`, and implicitly in the size, since an empty region is not one. Two fields for one fact can disagree (`valid` with a size of 0), and every consumer had to bind the flag to get at the fields after it.

**Change**
```cpp
// before
struct NextResult { bool valid; VirtAddr base; usize size; MmuFlags mmuf;
                    explicit operator bool() const { return valid; } };
// after
struct NextResult { VirtAddr base; usize size; MmuFlags mmuf;
                    explicit operator bool() const { return size != 0; } };
```

The size is the sentinel because it cannot be 0 for a real result: `next()` hands out only regions that intersect the range, clipped to it, so a clipped region is never empty, and the end is the value-initialized `{}`. The base could not be: any address, 0 included, can start a region, so a sentinel there would take a value away from real regions.

**Reference**
None, this project's own design; Zircon's `VmMapping::EnumerateProtectionRangesLocked()` hands each range to a callback instead of returning a result.

**Verification**
- Build, clang-format, clang-tidy: clean.
- `protection-ranges-smoke` (host) passes; `vmosnapshot.test`, `largepage.test`, `pagesource.test`, `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- `ustl::Option<Region>` as the result: the consumers would lose decomposing it in the `while` condition, which is why the aggregate exists.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why can a clipped region from `next()` never be empty?
- Why is the size a safe sentinel and the base not?
- What goes wrong when two fields of one result both say whether it is valid?
