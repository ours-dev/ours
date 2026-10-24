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
