# arch-paging changes

## The paging engine maps a 2 MiB leaf with a deposited table, and splits it without allocating (2026-10-24)

**Files**
- `kernel/lib/arch/include/arch/paging/deposits.hpp` (new), `kernel/lib/arch/BUILD.gn` — `TableDeposits`, `NoDeposits`, `DepositDiscipline`
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `map()`, `unmap()`, `protect()` and `free_tables()` take deposits; `split_ends()`, `split_at()`, `may_be_leaf()`; visitors are told the table that holds a leaf
- `kernel/lib/arch/include/arch/paging/paging_traits.hpp` — `SECOND_LEVEL`
- `kernel/lib/arch/test/x86_paging_smoke.cpp` — seven checks with a pool of deposits

**Symptom**
None: step 1 of `.agents/tasks/large-pages/`. The engine could map a large leaf but not take one apart: an unmap or protect of part of it answered InvArgs (`UnmapVisitor::on_terminal()`, `ProtectVisitor::on_terminal()`), so user memory could not be mapped with 2 MiB leaves without making those operations fail.

**Cause**
The objects:

- The engine, `Paging<X86Paging>`: walks the four levels for `map()`, `unmap()`, `protect()`, `query()` and `free_tables()`, and takes everything it needs from its caller (a `phys_to_table` translation, an allocator).
- A 2 MiB leaf: a second-level (x86 page directory) entry with PageSize set, standing for 512 small pages.
- A deposit (the user's decision D1): a zeroed table set aside when a leaf is mapped, kept by the caller and named by the engine only by the table that holds the leaf's entry (the owner). Splitting a leaf needs a table to put the 512 small entries in; with one set aside, the split cannot fail for want of memory, which is what lets an unmap under a VMO's lock, with no way back, go through a large leaf.

The events, with deposits:

1. `map()` of a range that fits a 2 MiB leaf asks `reserve(owner)` before it writes one. Granted, the leaf is written; refused, that part of the range is mapped with small entries in a table from the allocator, as before. No leaf above the second level (D3).
2. `unmap()` or `protect()` first splits the leaf that either end of the range falls inside (`split_ends()`): the two places a range can cover a leaf in part, since every leaf between them is covered whole. A split withdraws the owner's table, writes the 512 entries, and replaces the leaf with a pointer to that table.
3. The walk then meets only whole leaves. A whole leaf that `unmap()` clears gives one table back (`discard(owner)`).
4. `free_tables()` gives every table of a second-level table back (`discard_all()`) before it frees that table: the deposits of leaves still mapped at teardown.

Without deposits (`NoDeposits`), what the kernel's own mappings use, nothing changes: a leaf at any level that can hold one, and a part of one is InvArgs (D5).

**Change**
Each entry point gains a deposits argument, constrained by a concept; the old signatures forward with `NoDeposits{}`, so every existing caller compiles and behaves as before. The deposits interface, checked at compile time:

```cpp
template <typename T>
concept TableDeposits = requires(T &deposits, PhysAddr const owner) {
    { deposits.reserve(owner) } -> std::same_as<bool>;
    { deposits.withdraw(owner) } -> std::same_as<PhysAddr>;
    { deposits.discard(owner) } -> std::same_as<void>;
    { deposits.discard_all(owner) } -> std::same_as<void>;
};
```

The split, at the second level:

```cpp
auto const small = deposits.withdraw(pa_table);
auto &table = *reinterpret_cast<Table<FINAL> *>(phys_to_table(small));
auto const flags = MmuFlagsConv<ArchMmuFlags>::to_arch(
    MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags()));
for (usize i = 0; i < Traits::template NUM_PTES<FINAL>; ++i) {
    table[i] = Pte<FINAL>::make(entry.address() + i * PAGE_SIZE, flags, true);
}
std::atomic_thread_fence(std::memory_order_release);
entry = Pte<Level>::make(small, ArchMmuFlags{}, false);
```

Two details carry the correctness. The flags go through the neutral `MmuFlags` and back rather than being copied, because an entry's bits differ by level: bit 7 is PageSize in the leaf and PAT in a small entry, so a copy would quietly change the memory type of every page. And the table is filled before the entry that publishes it is written, with a release fence between, so a page walker on another CPU sees the old leaf or the full table, never a table half filled; on x86 the fence costs nothing but keeps the compiler from sinking the fill below the publish.

Splitting the ends first, rather than inside the visitors, keeps the visitors as they were: they still see only leaves they cover whole, and the one new thing they do is give a table back.

Rejected: a split inside the walk, at the leaf the visitor meets, which would need every visitor to know how to walk on into a table it just made; and a deposits flag on the engine's state instead of an argument, which would let a kernel walk and a user walk share one engine with different disciplines by accident.

**Reference**
Linux `pgtable_trans_huge_deposit()` and `pgtable_trans_huge_withdraw()` (`mm/pgtable-generic.c`, a FIFO hung off the PMD table page's `ptdesc->pmd_huge_pte`), `__split_huge_pmd_locked()` (fills the withdrawn table and publishes it with `pmd_populate()`), `zap_deposited_table()` (gives the table back when the huge PMD goes), and `vma_adjust_trans_huge()`, which splits the huge PMDs at the ends of a range before a VMA is cut there. Zircon splits by allocating instead (`X86PageTableImpl::SplitLargePage()`, `RemoveMapping()`), and unmaps the whole leaf when that fails in a user address space; the user chose Linux's way (D1).

**Verification**
- `x86-paging-smoke`, host, 14 checks pass, 7 of them new: a leaf with deposits reserves one table; with none available the range is small entries; an unmap of one page inside the leaf splits it without the allocator, leaves the other 511 as small entries to the same frames with the leaf's flags, and none of them has bit 7; a protect of the first page splits it and only that page loses Write; an unmap of the whole leaf gives its table back; a teardown with the leaf mapped gives it back; without deposits a part of a leaf is still InvArgs.
- Control runs, each with one piece undone: no split at the ends fails check 10; no discard on a whole-leaf unmap, 12; no `discard_all()` at teardown, 13; the flags copied raw, 10 (bit 7 found); a leaf written without asking `reserve()`, 8. Restored, all pass.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass. The kernel still calls only the `NoDeposits` signatures. clang-tidy reports nothing on the lines changed.

**Deliberately not done**
- Where deposits live and who allocates them, and the invalidation of the 2 MiB translation after a split: step 2 (D4).
- A test of the no-1-GiB rule (D3): it needs a 1 GiB range of small entries; the rule is the `Level != Traits::SECOND_LEVEL` branch of `may_be_leaf()`.
- The stale `kernel/lib/arch/test/paging_test.cpp`, which no build compiles.

**How it was found**
None: step 1 of `.agents/tasks/large-pages/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why is splitting only at the two ends of a range enough for an unmap of any length?
2. What would a CPU walking the page tables see if the compiler moved the fill of the new table below the store that publishes it, and why does x86 need no fence instruction for it?
3. Why are the split entries rebuilt from the neutral flags instead of copied from the leaf?
