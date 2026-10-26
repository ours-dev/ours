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
