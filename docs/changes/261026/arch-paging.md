# arch-paging changes

## Title: x86's page table gets a CRTP base for its walks, and query is the first walk in it (2026-10-26)

**Files**
- `kernel/arch/x86/lib/page_tables/` — new library (scaffolded with copier): `X86PageTableImpl<Derived>` with `query()`, level helpers that take the level as an argument, and host tests
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp` — `PageTable` derives from `X86PageTableImpl<PageTable>` and supplies its hooks
- `kernel/arch/x86/mm/page_table.cpp` — `query_mapping()` calls the base's `query()`; the hooks `table()` and `to_mmu_flags()`
- `kernel/arch/x86/kernel/BUILD.gn` — its headers depend on the new library
- `build/switch/kernel_switchset.gni` — the x86 kernel toolchains put the library's include directory on every translation unit
- `BUILD.gn` — the host tests in `//:default`
- `.agents/tasks/x86-page-table-crtp/DESIGN.md`, `PLAN.md` — D2 decided, step 2 done

**Symptom**
None, a change of structure the user decided (2026-10-25, D1 of `.agents/tasks/x86-page-table-crtp/DESIGN.md`): x86 walks its own page tables instead of driving the generic paging engine. The engine is shared by every architecture and by phys, so x86 shapes a walk only through cross-architecture traits, and it is instantiated once per level and once per kind of deposits: a map may run through 8.6 KB of code in 20 functions (stage 2 of `docs/perf/261025/page-walk.md`).

**Cause**
Two template parameters multiply the engine's code without changing what it does:

- **The level.** `map_range<Level>()`, `walk_range<Level>()` and `walk_levels<Level>()` are a function per level. What differs between levels of an x86 table is a shift and whether the page-size bit may be set: two numbers, not two functions.
- **The deposits type.** `PageTable::with_deposits()` calls every operation with `NoDeposits` or `PageTableDeposits`, so each exists twice although a table only ever uses one.

A base that takes the level as an argument and the derived class as its only template parameter is emitted once per kind of table.

**Change**
A library of its own, `kernel/arch/x86/lib/page_tables` (D2, option (b), the user's choice), with the same shape as `kernel/arch/x86/lib/idle_states`: header-only, built for the host where its tests run.

The base asks the derived class for what differs between kinds of table, through `static_cast<Derived const &>(*this)`, resolved at compile time:

```cpp
template <typename Derived>
class X86PageTableImpl {
  protected:
    auto query(VirtAddr const va) const -> ustl::Option<Mapping> {
        auto level = derived().top_level();
        auto table = derived().root();
        for (;;) {
            auto const entry = derived().table(table)[index(level, va)];
            if (!is_present(entry)) {
                return ustl::NONE;
            }
            if (is_leaf(level, entry)) {
                auto const size = page_size(level);
                return ustl::Option<Mapping>(Mapping{
                    .phys = address(entry) + (va & (size - 1)),
                    .size = size,
                    .flags = derived().to_mmu_flags(entry, level),
                });
            }
            table = address(entry);
            level = lower(level);
        }
    }
};
```

The walks are protected and take no lock: the derived class owns the root and the lock and calls them under it. That also keeps the base free of the kernel's lock types, so it builds for the host.

`PageTable` derives from it and supplies the hooks, PhysMap for `table()` and `MmuFlagsConv` for `to_mmu_flags()`:

```cpp
// query_mapping(), before
return Engine::query(m_pgd_phys, phys_to_virt, va);
// after
return query(va);
```

The other operations still drive the engine. Both write the same hardware format under the same lock, so the kernel runs on the mix until each has moved.

Two things the build needed, and why:

- **The include directory on every kernel translation unit.** `ours/arch/x86/page_table.hpp` is reached from most of the kernel through `vm_aspace.hpp`, not through GN dependencies. The kernel toolchains of `build/switch/kernel_switchset.gni` already list such headers in `prefix_configs` (gktl, lockdep, ktl); the library joins them, for x86 only.
- **The host tests named in `//:default` directly**, as `x86-paging-smoke` is, not through a `tests` group in the library's BUILD.gn. A group there loads that file in the default toolchain too, and with it `//kernel/lib/arch:headers`, whose config names `//kernel/mm`; `gn gen` then fails in phys_executable.gni, which only the host and kernel toolchains can evaluate.

**Reference**
Zircon `X86PageTableImpl<T>`, its `GetMapping()` (the walk, with the level as an argument) and `QueryVaddr()` (`kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`); `X86PageTableMmu` derives from it (`kernel/arch/x86/include/arch/aspace.h`). Zircon's base also holds the lock and the root; here the derived class does, so that the base builds for the host.

**Verification**
- `page_tables` host tests, 5 pass: nothing mapped, a 4 KiB leaf, its neighbour, 2 MiB and 1 GiB leaves, and the same tables read by the engine's `query()` and by the base's at eleven addresses, with the same answer at each.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 147 passed (146 and the new one).
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- Controls:
  - with the leaf check disabled in `query()`, 3 of the 5 host tests fail;
  - with `query()` answering "not mapped" for every address and the kernel rebuilt, both images panic at boot ("Failed to set readonly GDT"), so the kernel's queries go through the new walk. Restored, both boot.
- clang-tidy clean on `page_table.cpp` and the test; clang-format clean.
- Code size (`scripts/code_size.py`, the rel image): a query 481 to 423 bytes, the four levels a loop instead of four copies. A map unchanged, 8626 bytes.

**Deliberately not done**
- Map, unmap, protect and the rest: the next steps, one at a time.
- The lock and the root in the base, as in Zircon: the base would need the kernel's lock types and could not build for the host.
- The PTE format: the base uses the bits, `X86MmuFlags` and the level enum of `kernel/arch/x86/lib/arch`, which phys uses too, rather than a second definition of the hardware format.

**How it was found**
None: a new structure.

**The user's prediction**
None: homework is paused (2026-10-21).

**Three questions you should be able to answer**
- Why does a walk with the level as an argument emit less code than one with the level as a template parameter, and what does it cost per call?
- Why are the base's walks protected and lock-free, when Zircon's base holds the lock?
- Why did a `tests` group in the library's BUILD.gn break `gn gen`, when the same group works for `idle_states`?

## Title: unmap and protect walk through the CRTP base, and whether a table splits its large leaves is asked at run time (2026-10-26)

**Files**
- `kernel/arch/x86/lib/page_tables/include/page_tables/page_tables.hpp` — `unmap()`, `protect()`, the range walk `walk_range()` with its last-level loop, and `split()`; the hooks for deposits
- `kernel/arch/x86/lib/page_tables/include/page_tables/mmu_format.hpp` — new: `MmuFormat`, what the x86 MMU's entry bits mean, named by a derived class as its `Format`
- `kernel/arch/x86/lib/page_tables/BUILD.gn` — the new header
- `kernel/arch/x86/lib/page_tables/test/page_tables_test.cpp` — 10 new tests, among them one that runs the same calls through the engine and the base and compares every entry
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp` — `Format`, `splits_large_leaves()`, `withdraw_table()`, `discard_table()`; `to_mmu_flags()` moves into `MmuFormat`
- `kernel/arch/x86/mm/page_table.cpp` — `unmap_pages()` and `protect_pages()` call the base
- `.agents/tasks/x86-page-table-crtp/PLAN.md` — step 3 done

**Symptom**
None, step 3 of `.agents/tasks/x86-page-table-crtp/PLAN.md`. In the engine, unmap and protect were each emitted twice, once for `NoDeposits` and once for `PageTableDeposits`, and their walk once per level: 3232 and 3129 bytes of code in the rel kernel.

**Cause**
Whether a table splits its large leaves was a type, so every function that might split was a template on it, and `with_deposits()` instantiated each operation for both types. But the answer matters at one place, where the walk meets a 2 MiB leaf the range covers in part, and once more when a whole 2 MiB leaf is cleared. A question asked at two points of a walk does not need a second copy of the walk.

**Change**
The walk asks the derived class at run time:

```cpp
if (is_leaf(level, entry)) {
    if (offset == 0 && step == size) {
        action(entry, level, table);
        ...
        continue;
    }
    if (level != PageTableLevel::PageDirectory || !derived().splits_large_leaves()) {
        return ours::Status::InvArgs;
    }
    split(entry, table);
}
```

and `PageTable` answers from its `Flags`, with `PageTableDeposits` behind `withdraw_table()` and `discard_table()`. The walk is one function for the levels above the last, with the level an argument and the recursion through it, and a tight loop for the last level, as in the engine's stage 1 and 2. The two actions are lambdas: `unmap()` clears a leaf (and gives back a 2 MiB leaf's table), `protect()` ANDs its permission bits, so the walk is emitted once per action.

What an entry's bits mean moved into a type of its own, `MmuFormat`, which the derived class names as `Format`: what a leaf grants (`to_mmu_flags()`), a leaf and a table entry (`leaf()`, `table_entry()`), and an entry with other permissions (`with_permissions()`). It builds entries with `arch::paging::X86Pte`, so that the boot path's tables and these agree on the bits a leaf gets besides its permissions. Another kind of table (a guest's second-level table) would name another format. The host tests use the same `MmuFormat` as the kernel, so they test the kernel's format code, not a copy of it.

```cpp
// PageTable::protect_pages(), before
auto result = with_deposits([&](auto &deposits) {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    return Engine::protect(m_pgd_phys, phys_to_virt, deposits, va, n, flags);
});
// after
Guard<sync::SpinLock, IrqSave> guard(m_lock);
return protect(va, n, flags);
```

The engine's map still calls the engine's unmap for Undo; it moves with map.

**Reference**
Zircon `X86PageTableImpl::RemoveMapping()` and `RemoveMappingL0()`, `UpdateMapping()` and `UpdateMappingL0()`, and `SplitLargePage()`, with the level an argument (`kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`); its derived classes supply `terminal_flags()`, `intermediate_flags()` and `pt_flags_to_mmu_flags()`, which `MmuFormat` stands for here. Zircon allocates the table a split needs; here it comes from the leaf's deposit, so a split cannot fail.

**Verification**
- `page_tables` host tests, 15 pass. New: unmap of a range, across a table boundary, over a hole, of a whole 2 MiB leaf (its table given back), of part of one (split, the rest mapped to the same frames with the same permissions), of part of one without deposits (InvArgs, nothing changed), and of invalid ranges; protect taking a permission away and never giving one, and splitting; and the same six calls run through the engine and through the base on tables that split and tables that do not, after which every page from 0x3f0000 to 0xc00000 maps the same entry, bit for bit, the calls answered the same, and the same tables are set aside.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 147 passed.
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- Control: with the split turned off (every leaf covered in part InvArgs) and the kernel rebuilt, a host test fails and `largepage.test` gives no result in 120 s. Restored, all pass.
- clang-tidy clean on `page_table.cpp` and the test; clang-format clean.
- Code size (`scripts/code_size.py`, the rel image): unmap 3232 to 2183 bytes, protect 3129 to 1665, each one walk (686 and 769 bytes) instead of a copy per kind of deposits. Of what is left, `PageTableDeposits::withdraw()` and `discard()` are 540 and 551 bytes.

**Deliberately not done**
- Time: the host bench still drives the engine. The walk now calls itself once per level above the last, where the engine's was inlined whole into `PageTable`; what that costs a one-page call is measured when the bench moves to the base, at the end of the task.
- Map, `second_level_empty()`, `destroy()` and `alias_to()`: next steps.

**How it was found**
None: a planned step.

**The user's prediction**
None: homework is paused (2026-10-21).

**Three questions you should be able to answer**
- At which two points of an unmap does it matter whether the table splits its large leaves, and why is asking there enough?
- Why is the entry format a type the derived class names, rather than four more hooks on the derived class?
- What does a run-time level cost a one-page unmap that the engine's template level did not?

## Title: map and map_frames walk through the CRTP base, with one cursor for both (2026-10-26)

**Files**
- `kernel/arch/x86/lib/page_tables/include/page_tables/page_tables.hpp` — `map()`, `map_frames()`, their walk `map_range()` with its last-level loop, the failure handling `map_with()`, `may_be_leaf()`, and `MapCursor`; the hooks `allocate_table()` and `reserve_table()`
- `kernel/arch/x86/lib/page_tables/include/page_tables/mmu_format.hpp` — `leaf_bits()` and `leaf(pa, bits)`: a leaf's bits worked out once a call, and each frame ORed in
- `kernel/arch/x86/lib/page_tables/test/page_tables_test.cpp` — 12 new tests, among them the same maps run through the engine and the base
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp` — the two hooks
- `kernel/arch/x86/mm/page_table.cpp` — `map_pages()` and `map_pages_bulk()` call the base; `to_status()` gone with the last engine result it converted
- `.agents/tasks/x86-page-table-crtp/DESIGN.md`, `PLAN.md` — D3 decided, step 4 done

**Symptom**
None, step 4 of `.agents/tasks/x86-page-table-crtp/PLAN.md`. The engine's map was the largest of the walks: in the rel kernel a map could run through 8626 bytes in 20 functions, with 23 indirect calls (stage 2 of `docs/perf/261025/page-walk.md`).

**Cause**
Three things multiplied the engine's map:

- **The deposits type.** As for unmap and protect, every map function existed once for `NoDeposits` and once for `PageTableDeposits`.
- **The level.** `map_range<Level>()` was a function per level.
- **Functions passed as functions.** `PageTable` passed `phys_to_virt` and `alloc_table` to the engine as functions, and the engine's map was not inlined into `PageTable`, so each level reached its table and each allocation through a pointer: four indirect calls for a one-page map. The host bench passes a functor, which is inlined, so it never saw them.

**Change**
`map_range(table, level, cursor, flags, options)` is the engine's stage 2 walk with the level an argument: each table entered once, a tight loop at the last level, a present leaf skipped, an error or written over as the options say, an absent entry a leaf where the run fits one and `may_be_leaf()` allows it and a table from `allocate_table()` otherwise. Whether the table splits its large leaves is asked in `may_be_leaf()` (reserve a table for a 2 MiB leaf, or write none above the last level) and where a leaf covered in part is overwritten (split it). The hooks are member calls the compiler sees, so no call goes through a pointer.

One cursor serves both maps (D3, option (a), the user's choice): the frames are runs, one as long as the range for `map()` and one a page for `map_frames()`, as the engine's `MapContext` and Zircon's `MappingCursor` have them. The walk is emitted once; a page costs a check for the end of its run, which a contiguous map never reaches. A cursor per kind would have saved that check and emitted the walk twice.

```cpp
struct MapCursor {
    VirtAddr va;
    usize remaining;
    PhysAddr const *runs;
    usize run_size;
    usize run_used = 0;
    ...
};
// map()
MapCursor cursor{.va = va, .remaining = size, .runs = &pa, .run_size = size};
// map_frames()
MapCursor cursor{.va = va, .remaining = n * PAGE_SIZE, .runs = frames, .run_size = PAGE_SIZE};
```

A leaf's bits are worked out once a call (`Format::leaf_bits()`) and each frame is ORed in (`Format::leaf()`), as the engine converted the flags once into its `MapContext`. A failure part way is carried out as before (`map_with()`): Keep reports how far the call got, Undo unmaps it with the base's `unmap()`.

```cpp
// PageTable::map_pages(), before
auto result = with_deposits([&](auto &deposits) {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    return Engine::map(m_pgd_phys, phys_to_virt, alloc_table, deposits, va, pa, n, flags,
                       options, mapped);
});
// after
Guard<sync::SpinLock, IrqSave> guard(m_lock);
return map(va, pa, n, flags, options, mapped);
```

**Reference**
Zircon `X86PageTableImpl::AddMapping()` and `AddMappingL0()` with the level an argument, and `MappingCursor` (`kernel/vm/include/vm/mapping_cursor.h`), which holds an array of physical addresses and a page size the same way; `MapPagesContiguous()` builds one with a single address and the whole length as its page size. Zircon's `AddMapping()` takes an `ExistingEntryAction` (Error, Skip, Upgrade) where this takes MapOptions.

**Verification**
- `page_tables` host tests, 27 pass. New: the largest leaf each step allows (4 KiB, 2 MiB, 2 MiB, 4 KiB across a range); a 1 GiB range as one leaf without deposits and 512 2 MiB leaves with; Error with Keep and with Undo, Skip and Overwrite over a page already there; Overwrite splitting a 2 MiB leaf; NoMemory part way, undone and kept; invalid options and addresses; `map_frames()` a frame a page across a table boundary, and Skip stepping over a 2 MiB leaf and its 512 frames; and eight maps run through the engine and the base on tables that split and tables that do not, after which the calls answered the same, said they mapped the same, and every entry is the same, bit for bit.
- Control on the host: with no large leaf ever written, 3 tests fail, the comparison with the engine among them.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 147 passed.
- `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- Control in the kernel: with Overwrite treated as Error at the last level and the kernel rebuilt, `vmaphys.test` fails with 3 failed lines. Restored, all pass.
- clang-tidy clean on `page_table.cpp` and the test; clang-format clean.
- Code size (`scripts/code_size.py`, the rel image): a map 8626 to 4121 bytes, 20 functions to 8, 23 indirect calls to none; a bulk map 8692 to 4137. The walk is one function of 1267 bytes for every level.

**Deliberately not done**
- Time: measured when the bench moves to the base, at the end of the task.
- `second_level_empty()`, `destroy()` and `alias_to()`: next.

**How it was found**
None: a planned step.

**The user's prediction**
None: homework is paused (2026-10-21).

**Three questions you should be able to answer**
- How does one cursor serve a contiguous map and a map of scattered frames, and what does a page pay for it?
- Why did the kernel's map make indirect calls that unmap did not, and why does the base make none?
- With Skip, why may a map of scattered frames step over 512 frames at once, and what in the cursor handles it?
