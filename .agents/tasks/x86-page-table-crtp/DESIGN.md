# x86-page-table-crtp: x86 walks its own page tables through a CRTP base, not the generic engine

**Date**: 2026-10-25
**Status**: Accepted

## Problem
The x86 `PageTable` (`kernel/arch/x86/mm/page_table.cpp`) drives every walk through `arch::paging::Paging<X86Paging>` (`kernel/lib/arch/include/arch/paging/mod.hpp`), an engine shared by every architecture and by phys. The user wants x86 to walk its own tables, for two reasons (2026-10-25):

- **Control.** Each x86 step goes through cross-architecture traits, so x86 cannot shape a walk for itself.
- **Code that is generated twice.** Measured in the rel kernel with `llvm-nm`: the engine expands to 16 out-of-line functions, 7.8 KB. `map`, `map_frames`, `map_with` and `map_range` each exist twice, because `PageTable::with_deposits()` instantiates every operation for `NoDeposits` and for `PageTableDeposits`, and `map_range` is instantiated once per level. All page-table code, with `PageTable` and `ArchVmAspace`, is 19.6 KB, 2.1 % of the kernel's text; a page fault's map runs through about 3 KB of it.

What it does not fix: stage 2 of `docs/perf/261025/page-walk.md` made a one-page map slower (90 to 125 instructions) by walking a range table by table. Zircon's `AddMapping()` has the same shape, so this rewrite does not win that back (repair item 1 of `docs/repairs/261025/arch-paging.md` stays open).

The first page table of this repository had this shape, but its library was never committed and is not in any local checkout; only its wrapper (`MakePageTable`, in `1387a1a3`) survives. This is a rewrite after Zircon's, not a restore.

## Goals and non-goals
Goals:

- x86's MMU page table walks through a CRTP base, `X86PageTableImpl<Derived>`, with the x86-specific choices made by the derived class.
- One copy of each walk for the kinds of table, not for the deposits type: whether large leaves are split with deposits is a run-time property of the table, not a type. The level is a run-time argument in the walks that visit one entry a level, and a template parameter in the two that walk a range (D4).
- Every operation and option the engine serves today: map (contiguous and an array of frames), unmap, protect, query, `second_level_empty`, `destroy`, `alias_to`; Error, Skip or Overwrite, and Undo or Keep; deposits for splits.
- Measured as stage 3 of `docs/perf/261025/page-walk.md`, with code size added to the bench's time and instruction counts.

Non-goals:

- EPT: the base leaves room for a second derived class, which is not written.
- phys: it keeps the engine (decision D1). The engine then has phys and its own tests as users.
- The invalidation, shootdown, PCID and lock items (repair items 2 to 5).
- The one-page cost of a range walk (repair item 1).

## Design
Objects, after Zircon's `X86PageTableBase` and `X86PageTableImpl<T>` (`kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`) and `X86PageTableMmu` (`kernel/arch/x86/include/arch/aspace.h`):

- **`X86PageTableImpl<Derived>`:** the walks. `AddMapping(table, level, cursor)` for the levels above the last and `AddMappingL0(table, cursor)` for the last; the same pair for remove and update; `GetMapping()` for query. It asks `Derived` through `static_cast<Derived *>(this)` for what differs between page-table kinds: the top level, the flags of a leaf and of a table at a level, whether a level may hold a leaf, which addresses are valid, and how a changed entry is made visible.
- **The MMU derived class:** today's `PageTable` behind the same interface to `ArchVmAspace`, supplying the x86 MMU's answers, table allocation from the PMM and PhysMap's view of tables.
- **A mapping cursor:** the virtual address, the bytes left, and the physical frames (one run, or an array a frame a page), as Zircon's `MappingCursor` (`kernel/vm/include/vm/mapping_cursor.h`).
- **Deposits:** a split takes the table set aside for the leaf, as today (a split never allocates); whether the table splits at all is a flag fixed at creation, checked at run time.

Each operation moves from the engine to the base one at a time, the kernel booting on a mix in between: both write the same hardware format under the same lock.

## Alternatives considered
- **Keep the engine and remove the duplication in it** (deposits and level at run time): the same code-size gain for much less work, but x86 still works through cross-architecture traits, which is what the user wants to leave. Rejected by the user, 2026-10-25.
- **Move phys too and delete the engine:** one walker instead of two, but it widens the change to the boot path before the kernel side has proved itself. Deferred (D1).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Who walks x86's tables | (A) a CRTP base for x86, the engine kept for phys; (B) keep the engine, remove its duplication; (C) A, and move phys and delete the engine | A | None (homework paused) | A, 2026-10-25 |
| D2. Where the CRTP base lives | (a) the arch library, `kernel/lib/arch`, beside the x86 PTE format, built for host tests and the bench already; (b) a library of its own under `kernel/arch/x86`, as Zircon's `page_tables` | (a) | None | (b), 2026-10-26: `kernel/arch/x86/lib/page_tables` |
| D3. The cursor of a map | (a) one cursor for both maps, the frames as runs (one for a contiguous map, a page each for map_frames()), the walk emitted once; (b) a template on the kind of cursor, two walks, no end-of-run check for a contiguous map | (a) | None | (a), 2026-10-26 |
| D4. The level in the range walks (walk_range, map_range) | (a) a run-time argument, one function for all levels, the recursion a call a level; (b) a loop with an explicit stack, one function, no recursion; (c) a template parameter, a function a level and an action, the calls inlined | (c) | None | (c), 2026-10-27: a one-page call is 117 to 151 instructions instead of 249 to 279, at 400 to 1230 bytes more code a walk; (b) saved little (249 to 222) |

## Testing and verification
- The 29 checks of `x86-paging-smoke` ported to the new base, run on the host.
- `boot_check.py` (both images) and the kshell mapping tests (`largepage`, `vmosnapshot`, `pagesource`, `pager`, `vmaphys`, `vmaseal`, `vmaundo`, `usercopy`) after each operation moves.
- Stage 3 of `docs/perf/261025/page-walk.md`: the bench's times, lldb's instructions a call, and the bytes of code each operation runs through in the kernel, against stage 2.
- Not verifiable here: whether smaller code means fewer instruction-cache misses. That needs a hardware PMU.

## Open questions
None
