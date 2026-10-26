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
