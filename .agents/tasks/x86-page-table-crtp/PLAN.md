# x86-page-table-crtp: steps from the generic engine to x86's own CRTP page table

**Date**: 2026-10-25
**Status**: Proposed

## Request
The user, 2026-10-25: give `PageTable` back the CRTP shape and let it walk its own tables, not the paging engine, because the engine is hard to control and generates code twice for nothing, which does not help the cache. Option A of the design: x86 only, phys keeps the engine.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Who walks x86's tables | see DESIGN.md | A | None (homework paused) | A, 2026-10-25 |
| D2. Where the CRTP base lives | see DESIGN.md | (a) | None | (b), 2026-10-26 |
| D3. The cursor of a map | see DESIGN.md | (a) | None | (a), 2026-10-26 |

## Steps
Each step is introduced to the user before it starts, and lands in its own commit.

1. **Code size as a metric, and its stage 2 numbers.** A script that sizes, in the kernel's ELF, the code each page-table operation runs through (the function and everything it calls, by the call targets in its disassembly), written as samples `scripts/perf_stats.py` reads. Recorded in `docs/perf/261025/page-walk.md` as stage 2's code size, the numbers stage 3 is compared with. Why first: code size is the rewrite's main claim, and there is no baseline for it yet. Done: `code_size.py` and stage 2's code size in the report (a map 8626 bytes in 20 functions, 23 indirect calls; unmap 3232; protect 3129).
2. **The CRTP base, and query in it.** `kernel/arch/x86/lib/page_tables` with `X86PageTableImpl<Derived>` and `query()`, the level an argument; `PageTable` derives from it and `query_mapping()` calls it, the other operations still on the engine. Why query first: it is the smallest walk and writes nothing, so it settles the base's shape and hooks on its own. Done: 5 host tests, both images boot, a query 481 to 423 bytes.
3. **Unmap and protect.** One range walk in the base with two actions, splitting a 2 MiB leaf covered in part; whether a table splits its large leaves asked of the derived class at run time, and the entry format a type of its own (`MmuFormat`). Why now: map's Undo needs unmap, and the two share a walk. Done: 15 host tests, both images boot, unmap 3232 to 2183 bytes and protect 3129 to 1665.
4. **Map and map_frames.** One walk with the level an argument and one cursor for both (D3); the hooks `allocate_table()` and `reserve_table()`. Why now: map's Undo needs the base's unmap, now there, and map was the largest walk. Done: 27 host tests, both images boot, a map 8626 to 4121 bytes and no indirect call.
5. **The rest: second_level_empty, the teardown, the top level.** The base's `second_level_empty()` and `free_tables()` with the hooks `free_table()` and `discard_all_tables()`; `PageTable` uses the engine nowhere. Done: 31 host tests, both images boot; the control found that no kernel test destroys an address space with a 2 MiB leaf still mapped (`docs/repairs/261026/x86-mm.md`).
6. Later steps are introduced one at a time.

## Files
- `scripts/code_size.py`, `scripts/test_code_size.py` — new (step 1)
- `docs/perf/261025/page-walk.md` and `page-walk/stage-2-code-size.csv` — step 1
- `kernel/arch/x86/lib/page_tables/` — new (step 2)
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp`, `kernel/arch/x86/kernel/BUILD.gn`, `build/switch/kernel_switchset.gni`, `BUILD.gn` — step 2

## Test plan
- Step 1: `python3 scripts/test_code_size.py` passes; the script's totals agree with `llvm-nm -S` for a function that calls nothing; its output reads in `perf_stats.py`.
- Step 2: the `page_tables` host tests (including agreement with the engine's query), `boot_check.py`, and the kshell mapping tests.
- Step 5: the same, with `second_level_empty()` and the teardown against the engine's.
- Step 4: the same, with the engine and the base run on the same maps and compared entry by entry; controls on the host and in the kernel.
- Step 3: the same, with the engine and the base run on the same calls and compared entry by entry, and a control with the split turned off.
