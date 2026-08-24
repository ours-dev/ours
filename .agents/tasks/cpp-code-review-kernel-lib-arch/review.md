# C++ Code Review — `kernel/lib/arch`

- **Scope:** all 30 files under `kernel/lib/arch` (21 generic headers under `include/arch/`, 4 GN smoke tests under `test/`, the stale `tests/` CMake/gtest tree, `BUILD.gn`, `README.md`).
- **Date:** 2026-08-18.
- **Method note:** the cpp-code-review skill references Fuchsia docs under `docs/development/languages/c-cpp/`, which do not exist in this repo. I substituted the repo's own style authorities: `.agents/skills/language/modern-cpp-development/SKILL.md`, `.clang-format`, and `.clang-tidy`. I verified contracts against the x86 counterpart tree (`kernel/arch/x86/lib/arch`) because the generic code's correctness depends on it (`Pte`, `Traits`, `kAllLevels` ordering all check out there).
- **Verification run:** `clang-format --dry-run -Werror` (LLVM 22, repo config) reports **zero warnings on every file** in the directory. Formatting is clean; the findings below are semantic.
- **Heads-up:** the tree was being edited while I reviewed (a new `tlb.hpp`, an arch-dispatch refactor of `tick/halt/interrupt/random`, and a fixed `#endif` comment in `auto-interrupt-disabler.hpp` all landed mid-review). Findings reflect the tree as of 2026-08-18 ~15:00.

---

## What's done well

- **Static polymorphism throughout**: `Paging<ArchPaging>`, CRTP `RegisterLayout`/`RegisterBase`/`SysReg`, traits via `PagingTraits` composition — zero virtual functions, exactly matching the repo's "no virtuals for build-time-fixed architecture" rule.
- **Careful overflow checking in `Paging::map`** (`mod.hpp:44-57`): `n * page_size` overflow, `va + size` wraparound, and a phys-width check correctly guarded by `if constexpr` so the `1 << kMaxPhysAddrSize` shift can never be UB.
- **The visitor-shaped walk** (`mod.hpp:161-178`) cleanly unifies query/unmap/protect, and the comment explaining why `map` doesn't fit the shape is exactly the kind of "why" documentation worth keeping.
- The new **arch-completion dispatch pattern** (`tick.hpp:25-31` etc.) removes the include-order trap that `auto-interrupt-disabler.hpp` used to have to document.
- Everything is clang-format clean; `[[nodiscard]]` is used on most query-style APIs.

---

## High severity

### H1. `arch/options.hpp` and `arch/paging/options.hpp` cannot compile and are dead

`include/arch/options.hpp:8` and `include/arch/paging/options.hpp:8` both `#include <ustl/util/pack_options.hpp>` — **that file does not exist anywhere in the live tree** (`system/lib/ustl/include/ustl/util/` has no `pack_options.hpp`). Nothing in the tree includes either header (verified by grep), yet both are exported in `BUILD.gn`'s `public` list (lines 15 and 18).

Additionally `paging/options.hpp:10` reads `USTL_TYPE_OPTION(PageAllocator, PageAllocator);` which expands to a template struct **named `PageAllocator` containing a typedef also named `PageAllocator`** — self-shadowing even if the macro existed.

**Recommendation:** delete both headers and their `BUILD.gn` entries — they are unused. If an options-packing mechanism is needed later, design it fresh against the current ustl (and name the option types so they don't collide with the payload type); nothing here should be revived from archived trees.

### H2. `io/register_io.hpp` is an empty stub

`include/arch/io/register_io.hpp:6-7` is an empty `namespace arch::io {}`, included by nobody, but exported in `BUILD.gn:12`. Delete it or implement whatever it was reserving the name for.

### H3. The `tests/` directory is stale legacy that cannot compile

`tests/paging-test.cpp` does not match the current API at all:
- `MapError::None` (`tests/paging-test.cpp:167,169`) — the enum (`visitors.hpp:16-20`) has `InvalidArgument`, `Existent`, `NoMem` only.
- Treats the map result as an `Option` (`option.value() == MapError::None`) when `Paging::map` returns `ustl::Result<void, MapError>` (`mod.hpp:40`).
- `ArchPaging::Pte<Level>(value)` (`tests/paging-test.cpp:63,68`) — `X86Pte` is an aggregate with no such constructor.
- `table->size()` on `Table<Level>`, which is a raw C-array alias (`paging_traits.hpp:44`).
- Host STL (`<random>`, `<map>`, `std::string`), host gtest, CMake globbing (`tests/CMakeLists.txt`) — all inconsistent with the GN `test/` smoke pattern the repo actually builds.

It is superseded by `test/x86_paging_smoke.cpp`, which covers map/query/unmap/protect/large pages and matches the current API. **Recommendation:** remove `tests/` entirely.

### H4. No canonical-address validation in the paging entry points

`PagingTraits` faithfully re-exports `kVirtAddrExt` (`paging_traits.hpp:28` — for x86, `VirtAddrExt::Canonical`), but **nothing in `mod.hpp` ever consults it**. `map`/`unmap`/`protect`/`query` accept any `VirtAddr`, and `virt_to_index` (x86: `x86_pagings.hpp:96-101`) extracts only the per-level index bits (bits 12–47 for 4-level paging). Bits 48–63 are silently dropped, so e.g. `0x0001'8000'0000'0000` (non-canonical) would map/query exactly where `0x0000'8000'0000'0000` does. For a kernel whose upper layers ultimately map user-influenced addresses, this is a correctness hole: an invalid address should be `MapError::InvalidArgument`, not silently aliased.

**Recommendation:** validate `va` (and `va + size`) against `kVirtAddrExt` at the top of `map`/`unmap`/`protect`, next to the existing alignment checks (`mod.hpp:44`, `:78`, `:113`). The traits type is the natural home for a `static constexpr auto is_canonical(VirtAddr) -> bool` helper, implemented per `VirtAddrExt` variant.

### H5. `map()` failure leaves unreportable partial state

`map` walks and installs mappings in a loop (`mod.hpp:60-66`). On mid-range failure (`Existent` at `mod.hpp:284`, `NoMem` at `mod.hpp:300`) it returns only the error — **not how many pages were already mapped**, so the caller cannot unmap the partial range, and intermediate page tables freshly linked into the tree stay reachable. The higher layer clearly expects this information: `ArchVmAspace::map(..., usize *mapped)` (`kernel/arch/x86/mm/include/ours/mm/arch-vm-aspace.hpp:26`), and `MapContext` already tracks it (`num_mapped()`, `mapping_context.hpp:150-152`) — it just isn't threaded out.

**Recommendation:** return `ustl::Result<usize, MapError>` (pages mapped) like `unmap`/`protect` already do, or add an out-param, so callers can roll back on failure.

### H6. TLB-maintenance and PTE-write contracts are undocumented

`UnmapVisitor::on_terminal` zaps entries with `entry = {};` (`mod.hpp:231`) and `ProtectVisitor` rewrites them (`mod.hpp:264`), but the generic code performs — and nowhere documents — TLB responsibilities. The new `arch/tlb.hpp` offers only `tlb_invalidate_all()` (full flush; used by `kernel/arch/x86/mm/mmu.cpp:29`), with no per-page (`INVLPG`) or per-range primitive, and no guidance on when callers must flush relative to `unmap`/`protect`.

Also worth a comment: `entry = {}` is a plain non-atomic C++ assignment to memory that other CPUs may be concurrently reading in software (e.g. another `query`) — formally a data race per the C++ memory model (https://en.cppreference.com/w/cpp/language/multithread), acceptable in kernels only because aligned 8-byte stores are atomic on the target and hardware page walks aren't C++ threads. State these requirements explicitly in the `Pte` contract (atomicity, ordering, who flushes).

### H7. The `Allocator` contract (zeroed memory) is implicit and load-bearing

`map_one_from` allocates a sub-table, installs it as *present*, and immediately walks into it (`mod.hpp:296-313`). If the allocator ever returns non-zeroed memory, the walk reads garbage PTEs — catastrophic and hard to debug. The contract `Allocator(usize size, usize alignment) -> PhysAddr` (zeroed; `kTableAlignment`-aligned; `0` means failure — note `!entry_address` at `mod.hpp:299` also makes physical address 0 permanently unusable as a table) is documented nowhere. Write it down on `Paging::map`, ideally with a `static_assert`-style concept (see T1).

---

## Medium severity

### M1. `AutoInterruptDisabler` is implicitly copyable and movable

`auto-interrupt-disabler.hpp:14-34` defines a destructor but no copy/move operations, so the implicit copies exist: copying produces two guards that both `local_irq_restore_state` the same snapshot — a double-restore. This is exactly the [Rule of Three/Five](https://en.cppreference.com/w/cpp/language/rule_of_three) case. The repo already has the tool for this — `USTL_NO_MOVEABLE_AND_COPYABLE` is used on `ArchVmAspace` (`arch-vm-aspace.hpp:57`). Guards like this should be non-copyable and non-movable, full stop.

Related: `AutoInterruptDisabler(Defer)` (`:18`) leaves `m_state` default-initialized. Safe *today* only because x86's `ArchCpuState` happens to have `usize value_{};` — the neutral header should either initialize `IrqSavedState m_state{};` or document the NSDMI requirement on each arch.

### M2. `MapControl` / `HarvestControl` are disconnected from the generic paging API

`controls.hpp:10-19` defines `MapControl` and the mm layer threads it (`ArchVmAspace::map(..., MapControl, ...)`; `kernel/arch/x86/kernel/bootstrap.cpp:107` passes `ErrorIfExisting`), but `Paging::map` takes no control parameter: it hardcodes `MapError::Existent` on collision (`mod.hpp:284` — i.e. always `ErrorIfExisting`) and always opportunistically promotes large pages (`mod.hpp:289-291`) with no `TryLargePage` consultation. Either the controls are honored somewhere below `PageTable::map_pages` (I could not find any implementation of `map_pages` in-tree — it appears unimplemented), or the flags are silently dropped.

Also a design smell inside `controls.hpp`: `MapControl` mixes an **exclusive 3-way choice** (`SkipIfExisting = 0`, `OverwriteIfExisting = 1`, `ErrorIfExisting = 2`) with **OR-able bits** (`TryLargePage = BIT(2)`, `TryMerge = BIT(3)`) in one bitmask-enabled enum, so `SkipIfExisting | OverwriteIfExisting` is expressible but meaningless. Consider splitting into `enum class ExistingEntry { Skip, Overwrite, Error }` plus a separate bitmask, per the [bitmask-type idiom](https://en.cppreference.com/w/cpp/language/enum#Bitmask_types).

### M3. `map()`'s `page_size` parameter is dead

`mod.hpp:39` accepts `usize page_size = PAGE_SIZE` but `mod.hpp:44` rejects any other value with `InvalidArgument`. Large-page promotion is decided internally from alignment/size. Either remove the parameter or make it meaningful (e.g. "minimum granularity requested", tied to `TryLargePage`).

### M4. `unmap`/`protect` count pages *visited*, not pages *changed*

`on_absent` returns `ok(PAGE_SIZE)` (`mod.hpp:220-222`, `:253-255`), indistinguishable in the count from a page that was actually zapped/reprotected. `ArchVmAspace::unmap(..., usize *unmapped)` suggests the caller wants the real count. Consider tracking "entries actually modified" separately (a mutable counter on the visitor, or compare before/after).

### M5. Cursor structs: indeterminate defaults and unchecked dereference

`mapping_context.hpp` — `VirtAddrCursor() = default` (`:18`), `PhysAddrCursor() = default` (`:49`), `TravelContext() = default` (`:83`), `MapContext() = default` (`:135`) all leave raw members (`usize size_; usize consumed_; VirtAddr addr_; PhysAddr *addrs_;` at `:43-45`, `:74-77`) without initializers, so default-construct-then-read is UB ([indeterminate values](https://en.cppreference.com/w/cpp/language/default_initialization)). Add NSDMIs (`= 0`, `= nullptr`). Also `PhysAddrCursor::phys_addr()` (`:54-56`) dereferences `addrs_` without the `count_` guard that `remaining_size()` (`:67-72`) has, and `consume()` (`:58-65`) can underflow `count_` if called past the end — both worth `DEBUG_ASSERT`s.

### M6. Include hygiene in `mod.hpp`

`mod.hpp` uses `PAGE_SIZE` but gets it only transitively via `arch/types.hpp` → `ours/mm/cfg.hpp`. Include `ours/mm/cfg.hpp` directly so the dependency is visible where it's used.

---

## Minor / style

- **`types.hpp:17`** — `typedef usize PteVal;` → `using PteVal = usize;` (repo rule: alias over typedef). Comment typos: "follwoing", "grudually".
- **`mmu_flags.hpp:1`** — stray junk comment `// arch/paging ARCH/PAGING_MMU_FLAGS_HPP` above the license. `Discache` (`:19`) would read better as `CacheDisable`; `User` (BIT 4) is declared after `Discache` (BIT 5) — order by value.
- **`controls.hpp:34-35`** — `ResetUnaccessed`'s doc says *"If the frame is accessed we remove the accessed flag"* — the name should be `ResetAccessed`. Stray empty `///` at `:16`; trailing whitespace at `:31`.
- **`visitors.hpp`** — contains no visitors (`QueryResult`, `MapError`, `to_string` live here; the actual visitors are nested classes in `mod.hpp`). Misleading name; rename (`errors.hpp`?) or fold into `controls.hpp`. `to_string` (`:22`) is missing `[[nodiscard]]`, and its unbraced `case` bodies conflict with the repo's always-brace rule.
- **`register_addr.hpp:16`** — `using Self = RegisterAddr;` is unused. `addr()`, `value()`, and `RegisterLayout::get()` lack `[[nodiscard]]` (the repo's clang-tidy runs `modernize-use-nodiscard`).
- **`asm.hpp:6`** — `#include <arch/asm.hpp>` is a self-include (no-op under its own guard); remove it or point it at the intended header. The file also carries two parallel macro systems — legacy `LOCAL_FUNCTION`/`FUNCTION`/`DATA` (lines 9–20) and the vendored Zircon `.function`/`.object` machinery; plan to retire the legacy set.
- **`paging_dispatcher.hpp:36-37`** — the doc example is not valid C++ (`template <> PagingDispatcher<3, X86PageExt> { typedef X86ExtPaging Type; };` — specialization needs `struct`, and prefer `using`); typo "specilizations" at `:32`.
- **`mapping_context.hpp:3`** — guard `ARCH_PAGING_MAPPING_MAPPING_HPP` vs. filename `mapping_context.hpp`; several `typedef X Self;` (`:81`, `:132-133`) → `using`. `MappingAddress`, `skip()`, `take()`, `consumed_range()`, `set_cursor()` are unused in-tree — trim or exercise them.
- **Tests** — `cpu_mask_smoke.cpp` and `x86_feature_smoke.cpp` are missing the SPDX header every other file carries. `cpu_mask_smoke.cpp` tests `ours::CpuMask`, which is not part of this library — it belongs next to the cpu-mask code. `x86_feature_smoke.cpp:15-20` asserts `pcid && !has_feature(Pcid)` — tautologically false; the comment admits it, but the test proves nothing beyond "doesn't crash".
- **`sysreg.hpp:26,32`** — the `static_assert(!IsSameV<Tag, Tag>, ...)` always-false-dependent trick can become a plain `static_assert(false, ...)` now that the repo builds with C++23 ([P2593](https://en.cppreference.com/w/cpp/language/static_assert)); `= delete` on the two functions would give an even cleaner diagnostic.

---

## Teaching opportunities

### T1. Constrain the templates with concepts

The entire `ArchPaging` surface (`is_present`, `is_terminal`, `address`, `make`, `with_protection`, `ValueType`, `kAllLevels`, `virt_to_index`, `page_size`, `level_can_be_terminal`, …), the `PhysToVirt` and `Allocator` invocables, and the `Pte` per-level types are **implicitly** required. A misconfigured arch type fails deep inside `walk_levels` with a wall of instantiation errors. A C++20 `concept` in `paging_traits.hpp`:

```cpp
template <typename T>
concept ArchPagingLike = requires(T t, typename T::Level level, VirtAddr va, PhysAddr pa) {
    typename T::Level;
    typename T::template Pte<typename T::Level>;
    { T::virt_to_index(level, va) } -> std::convertible_to<usize>;
    { T::page_size(level) } -> std::convertible_to<usize>;
    { T::level_can_be_terminal(level) } -> std::convertible_to<bool>;
    // ...
};
```

would move the failure to the `Paging` class definition with a readable message, and doubles as the executable documentation for H7's allocator/zeroing contract (`requires { allocator(size, align) } -> std::convertible_to<PhysAddr>;`).

### T2. RAII guards and the Rule of Five

`AutoInterruptDisabler` (M1) is the canonical example for why a user-defined destructor should make you think about copy/move: the compiler-generated copy shares the saved state, and both destructors restore it. cppreference's [Rule of Three/Five/Zero](https://en.cppreference.com/w/cpp/language/rule_of_three) page lays out the decision table; for a guard whose whole job is "restore exactly once", deleting both is the only correct choice.

### T3. Bitmask enums: exclusive states don't belong in them

`MapControl` (M2) is a good illustration of the [bitmask type idiom](https://en.cppreference.com/w/cpp/language/enum#Bitmask_types)'s boundary: `|` only makes sense when every value denotes an independent, combinable property. A "pick exactly one" choice encoded as bits 0–2 invites `SkipIfExisting | OverwriteIfExisting` (value 3), which no `switch` will catch. Model exclusive choices as a plain scoped enum passed separately.

---

## Suggested follow-up order

1. Delete dead/broken surface: H1, H2, H3 (mechanical, unblocks everything else).
2. Harden the paging API contracts: H4 (canonicality), H5 (partial-progress reporting), H7 + T1 (document & constrain allocator/Pte contracts).
3. Fix `AutoInterruptDisabler` copyability (M1) — one-line `USTL_NO_MOVEABLE_AND_COPYABLE`.
4. Resolve the `MapControl` story (M2/M3/M4) when wiring `PageTable::map_pages`, which currently appears unimplemented.
5. Style/minor sweep (NSDMIs, naming, typos, `[[nodiscard]]`).
