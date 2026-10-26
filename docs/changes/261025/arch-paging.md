# arch-paging changes

## Title: the page table's map options are one of three existing-entry actions, Error first, and no longer ask for large pages (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/controls.hpp` — `MapOptions` loses `TryLargePage` and `TryMerge`, stops being a bitmask, and lists `ErrorIfExisting` first

**Symptom**
None that runs: every caller passes `ErrorIfExisting`, and the page table drops the options anyway (repair item 1 of `docs/repairs/261024/arch-paging.md`, which the next changes fix).

**Cause**
The enum mixed two kinds of thing in one bitmask, and got both wrong.

**The three existing-entry actions are a choice of one.** They were 0, 1 and 2, in a bitmask, with `SkipIfExisting` as 0. So a value nobody chose, or the empty mask, meant Skip: a map that silently leaves whatever was there. Error is the safe default; Skip and Overwrite are what a caller asks for when it knows the range may already be mapped.

**`TryLargePage` (bit 2) and `TryMerge` (bit 3) were requests for leaf sizes.**

- Bit 2 has the value 4, so it did not even sit apart from the three actions above, whose values run up to 2.
- No caller passes either bit anymore. Since the large-pages task, the engine picks the largest leaf each step of a range allows.
- `.agents/tasks/arch-leaf-sizes/DESIGN.md` proposes that this stays the engine's choice for good.

**Change**
```cpp
// before
enum class MapOptions {
    SkipIfExisting,
    OverwriteIfExisting,
    ErrorIfExisting,
    TryLargePage = 2_bit,
    TryMerge = 3_bit,
};
USTL_ENABLE_ENUM_BITMASK(MapOptions);
// after
enum class MapOptions {
    ErrorIfExisting,
    SkipIfExisting,
    OverwriteIfExisting,
};
```

The names stay, so no call site changes. The user chose on 2026-10-25 to implement all three actions, Overwrite included, over dropping the parameter. That comes in the next changes.

**Reference**
Zircon `ArchVmAspaceInterface::ExistingEntryAction` (`vm/include/vm/arch_vm_aspace.h`): `Skip`, `Error`, `Upgrade`, a plain enum. Zircon takes no leaf-size option either; `MapContiguous()` "is allowed to use any page size the architecture allows".

**Verification**
- Builds.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.

**Deliberately not done**
- Honouring the actions in the engine: the next changes.

**How it was found**
While fixing repair item 1 of `docs/repairs/261024/arch-paging.md`: the enum was read to decide what implementing it means.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- What did a `MapOptions` value of zero mean before this change, and why is that the wrong default for a map?
- Why are the existing-entry actions an enum and not bits?
- Who chooses a mapping's leaf sizes now, and why is that not a caller's option?

## Title: the page table honours the caller's choice of Error or Skip for a page already mapped, and of Undo or Keep for a failure part way (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/controls.hpp` — `MapOptions` is two groups of bits, each choice a bit of its own, with `map_options_valid()`: exactly one of each group
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `Paging::map()` takes the options and a `mapped` count. `map_one_from()` skips a present leaf for Skip. A failure is undone by unmapping what the call got through, or kept and reported.
- `kernel/arch/x86/mm/page_table.cpp` — `map_pages()` passes the options on. `map_pages_bulk()` maps each page with Keep and carries out the caller's failure choice over the whole call.
- `kernel/arch/x86/mm/arch_vm_aspace.cpp`, `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp` — `DEBUG_ASSERT` that the options are valid; a failed map with Undo is invalidated over its range
- `kernel/phys/aspace.cpp`, `kernel/mm/vm_mapping.cpp`, `kernel/mm/cpu_local.cpp`, `kernel/arch/x86/kernel/bootstrap.cpp`, `kernel/commands/large_page_test.cpp` — every caller names `ErrorIfExisting | UndoOnFailure`
- `kernel/lib/arch/test/x86_paging_smoke.cpp` — 7 new checks (15 to 21)
- `docs/repairs/261024/arch-paging.md` — item 1 narrowed to Overwrite

**Symptom**
Latent: every caller asked for Error, and the page table ignored the options (`(void)options`). Two things were wrong underneath, though no caller saw them:

- **The result lied after a failure.** A map that failed part way left the pages before the failure mapped. `map_pages()` reported `*mapped = 0` all the same, so the caller believed nothing was mapped.
- **There was no way to ask for anything else.** Skipping pages already mapped could not be asked for, and neither could keeping a partial map on purpose.

**Cause**
Repair item 1 of `docs/repairs/261024/arch-paging.md`: the options were declared with the interface and never implemented in the engine, which had no parameter for them.

Two decisions came first, both the user's (2026-10-25).

**What a failure part way leaves is the caller's choice.** Linux shows why no single answer fits:

- `remap_pfn_range()` zaps what it mapped: "A partial pfn range mapping is dangerous: it does not maintain page reference counts, and callers may free pages due to the error."
- `vm_insert_pages()` keeps it: "It is the caller's responsibility to account for this case."

**Both choices go in the one `MapOptions` value, as flags, and invalid combinations are refused.**

- Each choice is a bit of its own, and none is zero. A missing group, or two bits of one group, is detectable and is refused: InvArgs from the engine, and a `DEBUG_ASSERT` in the kernel.
- The first version of the enum had `SkipIfExisting` as 0, so leaving the action out silently meant Skip. Giving every choice its own bit closes that.

**Change**
```cpp
enum class MapOptions {
    ErrorIfExisting = 0_bit,
    SkipIfExisting = 1_bit,
    OverwriteIfExisting = 2_bit,
    UndoOnFailure = 3_bit,
    KeepOnFailure = 4_bit,
};
// exactly one bit of each group, nothing else
constexpr auto map_options_valid(MapOptions options) -> bool;
```

**In the engine:**

- **Skip.** A present leaf is stepped over: the rest of it, as far as the range goes. For a 2 MiB leaf that is one step of up to 2 MiB.
- **Error.** As before, `Existent`.
- **A failure.** The pages the call got through are counted from the virtual cursor.
  - With Keep, `*mapped` is that count.
  - With Undo, `unmap()` takes the same range back, and `*mapped` is 0.
  - With Skip, the undo takes the entries the call skipped there too. Zircon's `MapPages()` does the same (`RemoveMapping(cursor.ProcessedRange())`); a user mapping refaults them.

**In the x86 wrappers:**

- **`map_pages_bulk()`.** It maps one page per engine call, since the frames are not contiguous. It asks the engine to keep each page, and carries out the caller's choice over the whole call, under one hold of the lock.
- **`ArchVmAspace::map()` and `map_bulk()`.** They invalidate the range after a failed map with Undo. Between the write and the undo, another CPU running the address space may have cached an entry.

Overwrite is refused (InvArgs) until the next change.

**Reference**
- Zircon `ArchVmAspaceInterface::Map()` with `ExistingEntryAction` (`vm/include/vm/arch_vm_aspace.h`), which undoes on every failure. `X86PageTableBase::MapPages()` (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`) does the undo.
- Linux `remap_pfn_range_notrack()` undoes (`zap_vma_range()`), and `vm_insert_pages()` keeps (`mm/memory.c`).

**Verification**
- `x86-paging-smoke`, checks 15 to 21:
  - invalid options and Overwrite are refused before anything is mapped;
  - Error then Undo leaves nothing and the page that was there;
  - Error then Keep reports 2 and leaves pages 0 and 1;
  - Skip maps around a page that is there;
  - Skip steps over a whole 2 MiB leaf;
  - running out of tables at the third final-level table: Undo leaves nothing, and Keep reports 513.
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test` pass.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing on the changed lines.
- Control: with the undo's `unmap()` taken out, `x86-paging-smoke` exits 16 (Error then Undo leaves pages behind). Restored, 0.

**Deliberately not done**
- Overwrite: repair item 1 of `docs/repairs/261024/arch-paging.md`, next.
- A caller that asks for Skip or Keep: none needs one yet; every caller names `ErrorIfExisting | UndoOnFailure`.

**How it was found**
None: repair item 1 of `docs/repairs/261024/arch-paging.md`, worked through with the user.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why does every choice get a bit of its own, rather than one of them being zero?
- After a failed map with Undo, why must the TLB be invalidated, though every entry the undo removed was written by the same call?
- Why does `map_pages_bulk()` ask the engine to keep each page, and do the undo itself?

## Title: OverwriteIfExisting writes the new entry over the old one, splitting a large leaf it covers in part, and the TLB is invalidated before the map returns (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `map_one_from()` writes over a present leaf of the step's size, or splits a larger one and goes down into it; `split_leaf()` taken out of `split_at()` for both; Overwrite no longer refused
- `kernel/lib/arch/include/arch/paging/controls.hpp` — the option's comment says whose the invalidation is
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — a map with Overwrite is invalidated over its range, whatever its outcome
- `kernel/lib/arch/test/x86_paging_smoke.cpp` — checks 22 to 26; Overwrite leaves the invalid list of check 15
- `kernel/commands/vma_phys_test.cpp` — a page read, written over, and read again on one CPU; Error over it, 2 checks
- `docs/repairs/261024/arch-paging.md` — removed, its last item fixed

**Symptom**
None that runs: no caller asks for Overwrite yet. Until now the engine refused it with InvArgs.

**Cause**
The last part of repair item 1 of `docs/repairs/261024/arch-paging.md`. Overwrite is the one existing-entry action that changes a translation a CPU may already hold. That brings three questions the other actions do not raise:

- whose the TLB is;
- what happens to a large leaf the range covers only in part;
- what happens to a table where a leaf would go.

**Change**
The design the user agreed to on 2026-10-25. Objects:

- **A present leaf of the size the step maps.** Written over in place: `entry = Pte<Level>::make(pa, flags, true)`.
  - A 2 MiB leaf's deposit stays with it, since the new leaf is in the same slot and needs one as much.
  - x86 lets a present entry be changed directly. An architecture that needs break-before-make (arm64) would clear, invalidate and then write, in its own page table.
- **A larger leaf than the step.** The range covers it only in part, or the new frames are not aligned to it.
  - With deposits, it is split by `split_leaf()`, the code `split_at()` already used for unmap and protect, now shared. The walk then goes down into the new table and writes over its entries.
  - Without deposits, InvArgs, as an unmap of part of it is.
- **A table of small entries where a leaf would fit.** The walk goes down and writes over each entry, and the table stays. A leaf never takes a table's place: freeing a table would need the shootdown to retire it first, and unmap never frees one either.

Events, `ArchVmAspace::map()` with Overwrite:

1. The page table writes the entries, under its lock.
2. `invalidate(va, n)` runs on every CPU running the address space, whatever the outcome: an entry written over may be cached anywhere.
3. The map returns. Only now may the caller let an old frame go to anyone else.

The invalidation covers the whole range, not the entries that changed: the engine does not report them. That is performance items 3 and 4 of `docs/repairs/261025/arch-paging.md`.

With UndoOnFailure, a failure unmaps the prefix, the entries written over included. Their old contents are not restored; the next access refaults. Zircon's contract leaves the old mappings undefined in the same case.

**Reference**
Zircon `ArchVmAspaceInterface::ExistingEntryAction::Upgrade` (`vm/include/vm/arch_vm_aspace.h`, "the mapping can have its paddr changed"). Its x86 `X86PageTableBase::AddMapping()` updates the entry and queues the TLB invalidation in the `ConsistencyManager`, flushed before `MapPages()` returns.

**Verification**
- `x86-paging-smoke`, checks 22 to 26:
  - a page written over with another frame and other permissions;
  - one page inside a 2 MiB leaf: the leaf is split into its deposit with no allocation, and the other 511 stay;
  - a whole 2 MiB leaf written over in place: still a leaf, deposit kept;
  - a 2 MiB aligned run over a table of small entries: each entry written over, the table kept, no deposit reserved;
  - part of a leaf without deposits: InvArgs, the leaf unchanged.
- `vmaphys.test`, 8 checks (2 new):
  - on one CPU with preemption off, a page read (`0x0f1e2d3c4b5a6978`), written over with another frame, then read again (`0x8796a5b4c3d2e1f0`);
  - ErrorIfExisting over it is AlreadyExists.
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test` pass.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing on the changed lines.
- Control: without the invalidation after an Overwrite, `vmaphys.test` reads `0x0f1e2d3c4b5a6978` twice, the old frame through the cached translation, and fails. Restored, it passes.

**Deliberately not done**
- Invalidating only the entries that changed, or one `invlpg` per leaf: performance items 3 and 4 of `docs/repairs/261025/arch-paging.md`.
- Replacing a table with a leaf: never needed, and it would need a table freed after a shootdown.
- A caller of Overwrite in mm: none needs one yet.

**How it was found**
None: repair item 1 of `docs/repairs/261024/arch-paging.md`, finished.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why may the caller free the old frame only after the map returns, and what goes wrong on another CPU if it frees it sooner?
- Why does the control read the old value on the same CPU, though the entry was already written over?
- Why does Overwrite go down into a table of small entries instead of putting the 2 MiB leaf in its place?

## Title: unmap and protect walk their range once, table by table, and split a large leaf where the walk meets it; split_ends() is gone (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `walk_range()`, a walk over a range that enters each table once and steps through its entries; `ClearLeaf` and `NarrowLeaf`, what unmap and protect do to a leaf covered whole; `unmap()` and `protect()` on it; `split_ends()`, `split_at()`, `UnmapVisitor`, `ProtectVisitor` and `visit_range()` gone
- `kernel/lib/arch/test/x86_paging_smoke.cpp` — check 27: an unmap from inside one 2 MiB leaf to inside the next splits both
- `docs/repairs/261025/arch-paging.md` — item 7 removed, fixed; item 1 narrowed to map

**Symptom**
A one-page protect or unmap read 10 tables where map reads 4. A range of N pages read 4N tables where the floor is a handful: 2054 for 512 pages against 4. The one-page flame graph put 34 % of the samples in `split_at()` (`docs/perf/261025/page-walk.md`, baseline).

**Cause**
Two decisions, made two months apart, added up.

- **2026-08-18, `f1787422`.** The paging library made query, unmap and protect share one walk, `walk_levels()`, driven by a per-operation visitor: "instead of three near-identical walkers". That walk goes to one address. So a range became one walk from the root per leaf, and a visitor could act only on the leaf it was handed, never change the table's shape.
- **2026-10-24, `8a80d0ec`.** The large-pages task needed unmap and protect to split a 2 MiB leaf they cover in part. The visitor could not, so `split_ends()` was added before the walk: two more walks from the root, to the second level at `va` and at `va + size`, on every call, whether or not any leaf was there.

The first version of the page table, before `f1787422` and never committed, was Zircon's shape, as the user recalled. Zircon's `X86PageTableBase::RemoveMappingL*()` and `UpdateMappingL*()` loop over a table's entries, and split a large page covered in part as the loop meets it (`SplitLargePage()`).

**Change**
`walk_range<Level>(pa_table, phys_to_table, deposits, cursor, action)`, where the cursor is the next address and the bytes still to do. It enters the table once and loops over its entries from the cursor's index. For each entry it takes the part of the range under it:

- **absent:** passed over whole. Before, `on_absent()` stepped one page, so a hole of 1 GiB took 262144 steps.
- **a leaf covered whole:** `action.on_leaf<Level>(entry, pa_table)`. `ClearLeaf` clears it and discards a second-level leaf's deposit. `NarrowLeaf` ANDs its permission bits, as `ProtectVisitor` did.
- **a leaf covered in part:** with TableDeposits at the second level, `split_leaf()` and go down into the table it became; otherwise InvArgs, as before.
- **a table:** go down.

A leaf can be covered in part only at the two ends of a range, so the walk splits at most twice, and only where it already is.

The two operations share the walk, so there are still not three walkers. `map()` keeps its own walk for now; it is stage 2 (repair item 1).

```cpp
// unmap(), before
split_ends(table, phys_to_table, deposits, va, size);
while (remaining != 0) {
    UnmapVisitor<Deposits> visitor(address, remaining, deposits);
    auto result = walk_levels<Traits::PAGING_LEVEL>(table, phys_to_table, address, visitor);
    ...
}
// after
RangeCursor cursor{va, size};
ClearLeaf<std::remove_reference_t<Deposits>> action{deposits};
auto result = walk_range<Traits::PAGING_LEVEL>(table, phys_to_table, deposits, cursor, action);
```

The results are as before: `n` pages on success, and on an error whatever came before it is done.

**Reference**
Zircon `X86PageTableBase::RemoveMapping()` and `UpdateMapping()` with their `*L0()` and `SplitLargePage()` (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`). Linux `zap_pmd_range()` and `change_pmd_range()`, which call `__split_huge_pmd()` on the PMD they cover in part (`mm/memory.c`, `mm/mprotect.c`).

**Verification**
- `x86-paging-smoke`, 27 checks. The split checks (10 to 14), the Overwrite ones and the new 27 pass.
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- Control: with the walk not splitting a leaf whose part starts at the leaf's start (the end of a range), `x86-paging-smoke` exits 11 (a protect of a leaf's first page). Restored, 0.
- The gain is stage 1 of `docs/perf/261025/page-walk.md`:
  - protect and unmap of 512 and 32768 pages go from 0.94 to 0.98 ns a page to 0.49 to 0.51 ns, 1.93 times, at the floor of tables read;
  - one-page calls read 4 tables instead of 10, but gain only 10 to 13 % (protect 4.39 to 3.94 ns, unmap 4.78 to 4.17 ns), within the noise of one-page samples.

**Deliberately not done**
- `map()`'s walk: stage 2.
- Invalidating a prefix an unmap or protect changed before it failed. The x86 layer invalidates only on success, which this change leaves as it was; it is reachable only by a kernel range that covers part of a large leaf.

**How it was found**
`page-walk-bench`'s counts and flame graphs (`docs/perf/261025/page-walk.md`), then the user's memory that the first page table split as it walked, checked against Zircon's.

**The user's prediction**
The user expected much room in the engine. For ranges, that held: 1.93 times. I predicted a one-page protect or unmap would fall to the cost of a one-page map, about 2.6 ns, since `split_at()` had 34 % of the one-page flame graph. That did not hold: they fell by 10 to 13 %.

A share of a profile says where the time is spent, not what removing the code saves. The removed walks read cached tables, and an out-of-order core overlaps much of that with the work around it.

**Three questions you should be able to answer**
- Why can a leaf be covered in part only at the two ends of a range?
- What did unifying the walkers in `f1787422` cost, and why did it take a second change two months later to show?
- Why did removing a function that held 34 % of the samples save only about 10 %?

## Title: map walks its range once, table by table, and a bulk map of scattered frames is one engine call (2026-10-25)

**Files**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `map_range<Level>()` replaces `map_one_from()`; new `map_frames()`; `map_with()` carries out a failure for both; `walk_range()` gets a tight loop at the final level
- `kernel/arch/x86/mm/page_table.cpp` — `map_pages_bulk()` is one `Engine::map_frames()` call
- `kernel/lib/arch/test/x86_paging_smoke.cpp` — checks 28 and 29, `map_frames()` and its options
- `benches/kernel/lib/arch/page_walk_bench.cpp` — the bulk case maps each batch with one `map_frames()` call, as `map_pages_bulk()` now does
- `docs/repairs/261025/arch-paging.md` — items 1 and 2 fixed and removed; a new item 1 for the one-page cost this adds; the others renumbered 2 to 5

**Symptom**
Repair items 1 and 2 of `docs/repairs/261025/arch-paging.md`, measured in `docs/perf/261025/page-walk.md`:

- a map of N pages walked from the root N times, reading 4 tables a page, and cost 1.25 ns a page at 32768 pages, against 0.50 for a protect after stage 1;
- a bulk map of 32 scattered frames was one engine call a page, 3.49 ns a page, most of it the per-call work.

**Cause**
`map()` was the last user of the one-leaf walk. Each step of its loop called `map_one_from<PAGING_LEVEL>()`, which started at the root and went down to the one entry the step wrote. The upper levels change once per 512 pages, but were read for every page. And `map()` took one physical address, so frames that are not contiguous could only be mapped a call each: `PageTable::map_pages_bulk()` looped over the frames and called the engine for each, repeating the validation, the context and the walk from the root, and carried out the caller's failure choice itself.

`PhysAddrCursor` already held an array of frames; nothing passed it more than one.

**Change**
`map_range<Level>(pa_table, ..., options, context)` enters a table once and fills its entries in a loop from the context's address, up to the table's end or the range's:

- **at the final level, a tight loop:** a present entry is skipped, an error or written over, as the options say; then the entry is made and the context moves one page;
- **above it:** an absent entry becomes a leaf when the rest of the run fits one and `may_be_leaf()` allows it, and otherwise gets a table from the allocator, which the walk goes down into. A present leaf follows the options as `map_one_from()` did: skipped to its end, an error, written over in place when it fits, or split with its deposit and gone into. A present table is gone into.

```cpp
// map(), before: one walk from the root per leaf
while (context.has_more()) {
    auto result = map_one_from<Traits::PAGING_LEVEL>(table, phys_to_table, allocator,
                                                     deposits, options, context);
    ...
}
// after: one walk of the range
MapContext context(va, &pa, 1, size, flags);
return map_with(table, phys_to_table, allocator, deposits, options, mapped, va, n, context);
```

`map_frames(table, ..., va, frames, n, flags, options, mapped)` validates each frame and builds the context over the array, one page a frame, then runs the same walk. `map_with()` holds what both do after the walk: on failure it works out how far the walk got from the context's address and reports it (Keep) or unmaps it (Undo), as `map()` did before.

```cpp
// PageTable::map_pages_bulk(), before: an engine call per frame
for (usize i = 0; i < n; ++i) {
    auto result = Engine::map(m_pgd_phys, phys_to_virt, alloc_table, deposits,
                              va + i * PAGE_SIZE, pa[i], 1, flags, each);
    ...
}
// after
return Engine::map_frames(m_pgd_phys, phys_to_virt, alloc_table, deposits, va, pa, n, flags,
                          options, mapped);
```

`walk_range()` gets the same tight loop at the final level: every present entry there is a one-page leaf covered whole, so the loop calls the action and steps, with none of the upper levels' checks for a part or a table.

`walk_levels()` now serves only `query()`.

**Reference**
Zircon `X86PageTableBase::AddMapping()` and `AddMappingL0()`, `RemoveMappingL0()` and `UpdateMappingL0()`, and `ArchVmAspaceInterface::Map()`, which takes `paddr_t* phys, size_t count` into one `MappingCursor` (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`). Linux `remap_pte_range()` and `zap_pte_range()` (`mm/memory.c`) for the per-table loops.

**Verification**
- `x86-paging-smoke`, 29 checks, exit 0. New: 28 (`map_frames()` of four scattered frames maps each page to its own frame) and 29 (over a page already there: Error with Undo leaves nothing of the call, Error with Keep reports the two before it, Skip maps around it).
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- The gain is stage 2 of `docs/perf/261025/page-walk.md`, against stage 1:
  - map of 32768 pages 1.25 to 0.70 ns a page (1.78 times), reading the floor of tables;
  - bulk map of 32 scattered frames 3.49 to 0.67 ns a page (5.25 times), 4 tables a page to 0.12;
  - protect and unmap of 32768 pages 0.50 and 0.49 to 0.27 and 0.15 ns (the final-level loop);
  - a one-page map is slower: 2.56 to about 3.8 ns, 90 to 125 instructions a call (lldb). Protect and unmap of one page cost about 0.5 ns more. New repair item 1.

**Deliberately not done**
- A short path for one page, to win back the one-page cost: it is a decision for the user, with its candidates in repair item 1.
- `MapContext` as it is, with its array cursor, for `map()` too: a cursor of its own for a contiguous run is one of item 1's candidates.

**How it was found**
None: stage 2 of the report, chosen by the user after stage 1 ("try Zircon's way").

**The user's prediction**
None: homework is paused (2026-10-21). Item 1 expected map to fall to about half, as protect and unmap did in stage 1: it fell to 0.56 of stage 1 for 32768 pages.

**Three questions you should be able to answer**
- Why does a one-page map run more instructions in a walk that reads the same 4 tables?
- What did `map_pages_bulk()` repeat for every frame, and which of it does `map_frames()` do once?
- Why can the final level of `walk_range()` skip the checks for a part of a leaf and for a table?
