# x86-page-table-crtp: x86's page table walks through its own CRTP base, in half the engine's code and, on the host, its time or better

**Date**: 2026-10-27

## Delivered
- `defad38e` docs: the design and plan, D1 (option A: x86 only, phys keeps the engine) decided.
- `ba95bbba` feat(scripts): `scripts/code_size.py` sizes what an operation may run through, by its call closure in an ELF, and stage 2's code size is the baseline (step 1). Record: `docs/changes/261026/scripts.md`.
- `f4dc7e20` feat(x86): the library `kernel/arch/x86/lib/page_tables` (D2, option (b)) with `X86PageTableImpl<Derived>`; `PageTable` derives from it, and query is the first walk in it (step 2). Record: `docs/changes/261026/arch-paging.md`.
- `46c4ddf1` feat(x86): unmap and protect, one range walk with two actions, and `MmuFormat` for what the entry bits mean (step 3).
- `588d02df` feat(x86): map and `map_frames`, one walk and one cursor for both (D3), with `allocate_table()` and `reserve_table()` as hooks (step 4).
- `172cad3f` refactor(x86): `second_level_empty`, the teardown and the top level move over; `PageTable` uses the paging engine nowhere (step 5).
- `09c6a5d6`, `402cc5de` benches: `page-walk-bench` drives the new walks; `page-walk-bench-mirror` runs the same cases on a mirror of Zircon's x86 walks, with a `check` mode that reads every page back. Record: `docs/changes/261026/benches.md`.
- `5c29b352` perf(x86): a page-table entry is a type of its own (`Pte`), so a walk keeps its cursor in registers, and `split()` is out of line.
- `bc0ad87f`, `2b20df0a` perf(x86): protect narrows entries with two masks worked out once per call, captured as plain values.
- `4e564dd9` perf(x86): `walk_range` and `map_range` take the level as a template parameter (D4).
- `06cf4dca`, `77425f32`, `4f105377`, `c4896ebf`, `57e8cec4` docs(perf): stages 3 to 6 and the mirror comparison in `docs/perf/261025/page-walk.md`, each with its raw samples, folded stacks and flame graphs.
- `0310eb3a` docs(repairs): repair item 1 of `docs/repairs/261025/arch-paging.md` says what the traces show.
- Records of every change: `docs/changes/261026/arch-paging.md`. New repair file: `docs/repairs/261026/x86-mm.md`.

## Verification
At the final state (`4e564dd9`, then documents only):

- **Tests:** the library's 32 host tests pass, among them five that run the same calls through the paging engine and through the new base on tables that split their large leaves and tables that do not, and compare the answers and every entry bit for bit (query, unmap and protect, map, `second_level_empty`, the teardown), and one that checks the protect masks against the old conversion for 65536 entry and flags pairs. `page-walk-bench check` and `page-walk-bench-mirror check` pass.
- **Boot:** `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 147 passed. `largepage.test`, `vmosnapshot.test`, `pagesource.test`, `pager.test`, `vmaphys.test`, `vmaseal.test`, `vmaundo.test` and `usercopy.test` pass.
- **Controls** (a defect put in on purpose, the tests run, the code restored): every step that changed what the kernel does had one, and each was caught, in the host tests or in the kernel's: query always none (both images panic at boot); the split turned off (`largepage.test` fails); `discard_all_tables()` taken out (a host test fails; no kernel test does, see Found along the way); Overwrite treated as Error (`vmaphys.test`, 3 lines); protect never clearing Writable (`largepage.test` and `vmosnapshot.test`); a range walk never splitting (`largepage.test`).
- **Static checks:** clang-tidy clean on `page_table.cpp`, the library's test and the bench; clang-format clean on every changed line.
- **Code size** in the rel kernel (`scripts/code_size.py`, the call closure of each `ArchVmAspace` operation): map 8626 to 5244 bytes (39 % less), unmap 3232 to 2672 (17 %), protect 3129 to 2022 (35 %), query 481 to 423 (12 %); the engine's 23 indirect calls in a map are none.
- **Time and instructions**, on the host, taking turns with the engine's build, medians (`docs/perf/261025/page-walk.md`, stage 6):

  | | the engine | the new walks |
  |---|---|---|
  | map, 32768 pages, ns a page | 0.68 | 0.47 |
  | protect, 32768 pages | 0.26 | 0.26 |
  | unmap, 32768 pages | 0.14 | 0.14 |
  | bulk map, 32 frames | 0.67 | 1.06 |
  | one-page map, ns (instructions) | 3.8 (125) | 4.9 (151) |
  | one-page protect | 4.0 (141) | 3.5 (117) |
  | one-page unmap | 4.6 (152) | 3.9 (123) |

  Against the baseline before any change to the engine, a 32768-page map, protect and unmap cost 0.49, 0.27 and 0.15 ns a page, from 1.17, 0.96 and 0.94.
- **Not measured:** the instruction cache. Less code is measured; fewer misses are not, and need a hardware PMU. The times above are those of the walks compiled for the arm64 host (Apple M5 Pro), not x86-64 code, and leave out what the kernel adds around them: the lock, interrupts off, the TLB shootdown. Nothing was timed in the kernel or on x86 hardware.

## Deviations from the design
- **The level.** DESIGN.md made it a run-time argument, so that each walk is emitted once. Measured, that doubled the instructions of a one-page call, the kind a page fault makes; D4 made it a template parameter in `walk_range` and `map_range` (code up 20 to 31 % for map, unmap and protect), and left it an argument in the walks that visit one entry a level.
- **Tests.** The design planned to port the 29 checks of `x86-paging-smoke`. They were not: the engine keeps them, and the new base has 32 host tests of its own that compare it with the engine instead.
- **Measurement.** The design put the comparison in one stage. It became stages 3 to 6, because the first measurement found the new walks two to three times slower, and each cause found (aliasing between entries and cursors, a split's constants made on every call, the permission conversion done per entry, a call a level) was fixed and measured in turn. PLAN.md gained steps 6 and 7.
- **The mirror of Zircon's walks** (`page-walk-bench-mirror`) was not in the design; the user asked for Zircon's code to be tried as it is. It was no faster with the kernel's types.
- **The bench no longer measures the engine.** `page-walk-bench` drives the new walks; the engine's numbers come from a build of `ba95bbba` in a worktree.

## Found along the way
- **No kernel test destroys an address space with a 2 MiB leaf still mapped**, so the teardown giving back the table set aside for it is tested on the host only: `docs/repairs/261026/x86-mm.md` item 1.
- **`uint64_t` is `unsigned long long` on the host and `unsigned long` on x86-64.** The mirror's first run, with `u64` entries, was 2.4 times faster than the kernel's types would be, because the entries and the cursors were then types apart. Kept as `page-walk/mirror-u64*.csv`.
- **Capturing two masks in a struct was slower than as two values** (7905 against 4472 instructions for a 512-page protect), for a reason a small example did not reproduce. Kept as `page-walk/stage-5-struct-capture.csv`; the cause is not known.
- **lldb puts a breakpoint after a function's own prologue**, so an instruction count taken that way leaves the entry function's prologue out.
- **Corrections on the way, so that nobody repeats them:** `bc0ad87f`'s message quoted an exploratory run's count and not its own code's, and `2b20df0a` corrects it; a loop read in a disassembly was taken for protect's and was `split_leaf()`'s fill; two explanations of the one-page map's remaining cost (the second function, the frames read through a plain pointer) were tested and rejected, and the repair item was corrected (`0310eb3a`). Counts of executed instructions and controls that change one thing settled each; a reading of a disassembly did not.

## Not done
- **The bulk map (1.06 against 0.67 ns a page) and the one-page map (4.9 against 3.8 ns)** are slower than the engine's. Measured and partly explained, not fixed: `docs/repairs/261025/arch-paging.md` item 1, whose next step is to compile the walk to LLVM IR and see where the cursor's address leaves the function. Neither is more than a few nanoseconds in a path that zeroes a page and enters the kernel.
- **The paging engine** stays for phys (D1) and for `x86-paging-smoke`; its map, unmap, protect and teardown are no longer used by the kernel proper. Whether to prune them is the user's decision.
- **Invalidation, shootdown, PCID and interrupts off for a whole range:** `docs/repairs/261025/arch-paging.md` items 2 to 5, which need the kernel and, for their time, hardware.
- **An EPT page table** as a second derived class: the base leaves room for it, none is written.
- **Moving DESIGN.md to `docs/design/`** as an accepted design that outlives the task.
- **The comparison worktree** `.worktrees/exp-page-walk-stage1` (a build of `ba95bbba`, the engine's bench) is still there; `git worktree remove` takes it away when no more comparisons are wanted.
