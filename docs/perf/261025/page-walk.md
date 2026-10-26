# page-walk: performance report

**Date**: 2026-10-25
**Status**: Optimizing

## Question
Whether the paging engine's walks are worth making faster. Repair items 1 and 2 of `docs/repairs/261025/arch-paging.md` say:

- **Item 1:** every leaf of a range is found by a walk from the root.
- **Item 2:** a bulk map is one engine call per page.

The user expected a large room for optimisation there, asked for the data first, and then for flame graphs to judge it by (2026-10-25).

## Method
`page-walk-bench` (`benches/kernel/lib/arch/page_walk_bench.cpp`) calls the engine, `arch::Paging` with x86's four levels, on the host, on page tables in an arena whose addresses stand in for physical ones. The engine is header-only, and its walks are software on ordinary memory with no TLB in them, so the host's time is a fair number for that work. It leaves out what the kernel adds around them: the lock, interrupts off, and the shootdown.

- **Machine and build:** Apple M5 Pro, macOS 27.0.1, clang 23.1.0-rc3, host toolchain of `out/bringup.x86`.
- **Engine calls:** each call is made through a wrapper the compiler may not inline (`engine_map()`, `engine_protect()`, `engine_unmap()`), so that a profile can tell the three apart. The options are then a run-time argument, as they are in the kernel's `PageTable::map_pages()`.
- **Samples:** `page-walk-bench csv`, run three times, 32 samples a case in each, 96 in all, kept in `page-walk/baseline.csv`.
  - Each sample starts from empty tables and covers 32768 pages: it maps, protects and unmaps `32768 / n` ranges of `n` pages, each phase timed as a whole. The host clock ticks every 41.7 ns, longer than one operation on one page.
  - A full untimed pass comes first; without it the first run was up to 60 % slower.
- **Cases:**
  - 4 KiB leaves, with frames a page off a 2 MiB boundary, for 1, 512 and 32768 pages;
  - 2 MiB leaves, with aligned frames and deposits always granted, for 512 and 32768 pages;
  - a 32-frame bulk map done as `PageTable::map_pages_bulk()` does it, one engine call a page.
- **Metrics:**
  - `visits/page`: the tables the engine enters per page, counted by the `PhysToVirt` it is given. Exact on any machine.
  - `ns/page`: host time per page.
  - Two yardsticks: zeroing a 4 KiB page (1024 distinct pages a sample), and storing an entry with no walk.
- **The floor:** the distinct tables a range touches, which a walk going down once per table would read. 4 for one page; 4 for 512 pages; 67 for 32768 pages at 4 KiB; 3 for 2 MiB leaves.
- **Flame graphs:** `page-walk-bench spin 7 CASE`, sampled by macOS's `/usr/bin/sample` every millisecond for 5 seconds (`python3 scripts/flamegraph.py record`).
  - The folded stacks are the raw data: `page-walk/baseline-4k-1.folded`, `baseline-4k-32768.folded`, `baseline-bulk-32.folded`, about 3850 samples each.
  - The pages beside them are made from them (`scripts/flamegraph.py render`).
  - The engine is inlined into each wrapper, so a wrapper's own share is its walk, and only out-of-line functions (`split_at`, `unmap`'s recursion) show as frames of their own.
  - About 3 % of samples lack their caller's frame: they caught a wrapper before its frame was set up.
- **Code size, from stage 2 on:** the bytes of code each operation of the kernel's x86 page table may run through, measured in the rel image's ELF (`kernel_x86.dbg.0/.../vmours.with-tests`, -O2) by `scripts/code_size.py`. Added for the CRTP rewrite (`.agents/tasks/x86-page-table-crtp`), whose main claim is less code.
  - An operation is sized by its closure: its `ArchVmAspace` entry and every function that can be reached from it by a direct call, a jump to another function or a function's address it takes, each counted once. A single function's size would not do: the compiler inlines the engine into `PageTable::unmap_pages()` and leaves `map_range()` out of line, so the same walk sits in one function or in five.
  - The closure stays inside the page-table code (`PageTable`, `ArchVmAspace`, `PageTableDeposits`, the engine and `phys_to_virt()`); table allocation and freeing, the lock and the shootdown are outside and are reported as external callees.
  - It is code that may run, error paths and both kinds of table included, not code one call runs: that is the instruction count. Whether less code means fewer instruction-cache misses needs a hardware PMU, and is not measured.
  - Metrics: `code bytes`, `functions`, `indirect` (calls and jumps through a register, which the closure cannot follow), `external` (distinct callees outside). One sample a case: the build is deterministic.

## Baseline
`page-walk/baseline.csv`, statistics by `python3 scripts/perf_stats.py docs/perf/261025/page-walk/baseline.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 2.40 | 2.62 | 3.30 | 3.88 | 2.79 | 0.38 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 3.91 | 4.39 | 5.35 | 7.22 | 4.62 | 0.63 |
| protect 4K 1 | visits/page | tables | 96 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 4.13 | 4.78 | 5.64 | 7.85 | 4.93 | 0.68 |
| unmap 4K 1 | visits/page | tables | 96 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| map 4K 512 | ns/page | ns | 96 | 1.17 | 1.18 | 1.49 | 1.54 | 1.28 | 0.14 |
| map 4K 512 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 0.97 | 0.98 | 1.23 | 1.32 | 1.06 | 0.12 |
| protect 4K 512 | visits/page | tables | 96 | 4.01 | 4.01 | 4.01 | 4.01 | 4.01 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 0.95 | 0.96 | 1.20 | 1.69 | 1.05 | 0.13 |
| unmap 4K 512 | visits/page | tables | 96 | 4.01 | 4.01 | 4.01 | 4.01 | 4.01 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 1.16 | 1.17 | 1.40 | 1.55 | 1.26 | 0.12 |
| map 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 0.96 | 0.96 | 1.17 | 1.32 | 1.04 | 0.10 |
| protect 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 0.93 | 0.94 | 1.13 | 1.72 | 1.02 | 0.12 |
| unmap 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0025 | 0.0051 | 0.0063 | 0.0089 | 0.0052 | 0.000951 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0076 | 0.0089 | 0.0102 | 0.00784 | 0.00102 |
| protect 2M 512 | visits/page | tables | 96 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0089 | 0.0102 | 0.0114 | 0.00834 | 0.00118 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0038 | 0.0051 | 0.00282 | 0.000674 |
| map 2M 32768 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0026 | 0.0038 | 0.0022 | 0.000609 |
| protect 2M 32768 | visits/page | tables | 96 | 0.006 | 0.006 | 0.006 | 0.006 | 0.006 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0026 | 0.0038 | 0.00225 | 0.00061 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.006 | 0.006 | 0.006 | 0.006 | 0.006 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 3.26 | 3.27 | 3.92 | 4.41 | 3.49 | 0.31 |
| bulk-map 4K 32 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 14.1 | 15.0 | 17.2 | 29.8 | 15.9 | 1.86 |
| yardstick store-entry | ns/page | ns | 96 | 0.0699 | 0.0725 | 0.0827 | 0.12 | 0.0764 | 0.00757 |

The same work with the engine inlined into the bench, before the wrappers (`page-walk/baseline-inlined.csv`), by `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/baseline-inlined.csv docs/perf/261025/page-walk/baseline.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 1.68 | 2.62 | 0.64x | +56.1% |
| protect 4K 1 | ns/page | ns | 3.51 | 4.39 | 0.80x (within noise) | +25.2% |
| unmap 4K 1 | ns/page | ns | 3.92 | 4.78 | 0.82x | +22.1% |
| map 4K 512 | ns/page | ns | 1.18 | 1.18 | 1.00x (within noise) | +0.1% |
| protect 4K 512 | ns/page | ns | 1.00 | 0.98 | 1.02x (within noise) | -2.4% |
| unmap 4K 512 | ns/page | ns | 0.95 | 0.96 | 1.00x (within noise) | +0.3% |
| map 4K 32768 | ns/page | ns | 1.17 | 1.17 | 1.00x (within noise) | +0.1% |
| protect 4K 32768 | ns/page | ns | 1.01 | 0.96 | 1.04x (within noise) | -4.2% |
| unmap 4K 32768 | ns/page | ns | 0.94 | 0.94 | 1.00x (within noise) | +0.1% |
| map 2M 512 | ns/page | ns | 0.0038 | 0.0051 | 0.75x | +34.2% |
| protect 2M 512 | ns/page | ns | 0.0076 | 0.0076 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0076 | 0.0089 | 0.85x (within noise) | +17.1% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 1.10 | 3.27 | 0.34x | +197.0% |
| yardstick zero-page | ns/page | ns | 15.0 | 15.0 | 1.00x (within noise) | -0.0% |
| yardstick store-entry | ns/page | ns | 0.0712 | 0.0725 | 0.98x (within noise) | +1.8% |

Flame graphs, shares of all samples:

- **`4k-1`: one page per call, 32768 calls a sample.**
  - `engine_map` takes 20.6 %, `engine_protect` 36.5 % and `engine_unmap` 38.4 %.
  - Inside them, `Paging::split_at<L3>` takes 34.0 % of all samples: 16.3 % under protect and 17.7 % under unmap. That is `split_ends()`, which walks to the second level twice per call to look for a large leaf to split. Here it finds none, and costs about half of each protect and unmap.
- **`4k-32768`: 32768 pages per call.** map 37.8 %, protect 30.2 %, unmap 28.8 %. `split_at` no longer shows: two walks per call, against 32768 pages.
- **`bulk-32`: 32 frames, one engine call a page.** `engine_map` takes 96.5 %.

What it says:

- **The walk is about 1 ns a page, and redoing it from the root is cheap.** At 32768 pages, map, protect and unmap cost 1.17, 0.96 and 0.94 ns a page at the median (p90 1.40, 1.17 and 1.13), against 4 visits a page where the floor is 67 for the whole range. The re-walked upper tables stay in the cache. A walk once per table would save at most about 1 ns a page, against 15.0 ns to zero the page a fault allocates.
- **What costs is per call.**
  - With the call out of line, as in the kernel, a one-page map costs 2.62 ns, a protect 4.39 ns and an unmap 4.78 ns.
  - The bulk map costs 3.27 ns a page, 2.8 times the contiguous map.
  - The first comparison shows how much of that the compiler took away when it could see the call: the one-page map went from 2.62 to 1.68 ns, and the bulk map from 3.27 to 1.10 ns. It folded the constant options and the setup into the caller.
- **About half of a one-page protect or unmap is `split_ends()`** finding nothing to split. That is the largest single target the data shows, and it is not one the repair items name.

Candidates the data points at, for the user to decide (none started):

- **Skip `split_ends()` where no leaf can be split.** For example, when both ends are 2 MiB aligned. Or fold the check into the walk the operation makes anyway: it reaches the second level on its own.
- **Item 2: map a bulk batch in one engine call**, which removes the per-call cost for 31 of every 32 pages.
- **Item 1: walk once per table.** The smallest gain.

## Stages

### Stage 1: unmap and protect walk their range once

- **Commit:** `54148303`. `walk_range()` enters each table once and steps through its entries, passes an absent span whole, and splits a large leaf covered in part where it meets it. `split_ends()` and the per-leaf walk of unmap and protect are gone. Map is unchanged.
- **Decision:** the user's, 2026-10-25. Return unmap and protect to the shape of the first page table, which followed Zircon's `RemoveMapping()`, after the baseline's flame graphs; map to follow as stage 2.
- **Predictions:**
  - The user expected large room in the engine.
  - I predicted that a one-page protect or unmap would fall to the cost of a one-page map, about 2.6 ns, since `split_at()` held 34 % of the one-page flame graph.
- **Samples:** `page-walk/stage-1.csv`, recorded as the baseline was: three runs, 96 samples a case, the same wrappers.

Statistics, `python3 scripts/perf_stats.py docs/perf/261025/page-walk/stage-1.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 2.40 | 2.56 | 3.45 | 3.60 | 2.80 | 0.38 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 3.70 | 3.94 | 5.24 | 5.72 | 4.31 | 0.61 |
| protect 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 3.92 | 4.17 | 5.88 | 6.57 | 4.68 | 0.78 |
| unmap 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 4K 512 | ns/page | ns | 96 | 1.18 | 1.27 | 1.50 | 1.60 | 1.35 | 0.12 |
| map 4K 512 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 0.47 | 0.51 | 0.60 | 0.72 | 0.54 | 0.0544 |
| protect 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 0.46 | 0.49 | 0.71 | 0.79 | 0.57 | 0.11 |
| unmap 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 1.19 | 1.25 | 1.48 | 1.71 | 1.33 | 0.12 |
| map 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 0.49 | 0.50 | 0.59 | 1.00 | 0.54 | 0.071 |
| protect 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 0.48 | 0.49 | 0.70 | 0.70 | 0.56 | 0.10 |
| unmap 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0038 | 0.0051 | 0.0064 | 0.0076 | 0.00541 | 0.000661 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0076 | 0.0089 | 0.0089 | 0.00729 | 0.000965 |
| protect 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0076 | 0.0089 | 0.0102 | 0.00775 | 0.000953 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0025 | 0.0025 | 0.0038 | 0.0038 | 0.00283 | 0.000548 |
| map 2M 32768 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0013 | 0.0025 | 0.0051 | 0.00158 | 0.000637 |
| protect 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0013 | 0.0019 | 0.0026 | 0.00143 | 0.000384 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 3.30 | 3.49 | 4.15 | 4.38 | 3.66 | 0.30 |
| bulk-map 4K 32 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 15.0 | 15.2 | 18.0 | 21.1 | 16.3 | 1.53 |
| yardstick store-entry | ns/page | ns | 96 | 0.0699 | 0.0712 | 0.0858 | 0.11 | 0.0771 | 0.00836 |

Gain over the baseline, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/baseline.csv docs/perf/261025/page-walk/stage-1.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 2.62 | 2.56 | 1.02x (within noise) | -2.2% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 4.39 | 3.94 | 1.12x (within noise) | -10.4% |
| protect 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| unmap 4K 1 | ns/page | ns | 4.78 | 4.17 | 1.15x (within noise) | -12.9% |
| unmap 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| map 4K 512 | ns/page | ns | 1.18 | 1.27 | 0.93x (within noise) | +8.1% |
| map 4K 512 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 512 | ns/page | ns | 0.98 | 0.51 | 1.93x | -48.1% |
| protect 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| unmap 4K 512 | ns/page | ns | 0.96 | 0.49 | 1.94x | -48.5% |
| unmap 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| map 4K 32768 | ns/page | ns | 1.17 | 1.25 | 0.93x (within noise) | +7.3% |
| map 4K 32768 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 32768 | ns/page | ns | 0.96 | 0.50 | 1.92x | -47.9% |
| protect 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| unmap 4K 32768 | ns/page | ns | 0.94 | 0.49 | 1.94x | -48.4% |
| unmap 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| map 2M 512 | ns/page | ns | 0.0051 | 0.0051 | 1.00x (within noise) | +0.0% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0076 | 0.0076 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| unmap 2M 512 | ns/page | ns | 0.0089 | 0.0076 | 1.17x (within noise) | -14.6% |
| unmap 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0025 | 0.0013 | 1.92x (within noise) | -48.0% |
| protect 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| unmap 2M 32768 | ns/page | ns | 0.0025 | 0.0013 | 1.92x | -48.0% |
| unmap 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| bulk-map 4K 32 | ns/page | ns | 3.27 | 3.49 | 0.94x (within noise) | +6.8% |
| bulk-map 4K 32 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| yardstick zero-page | ns/page | ns | 15.0 | 15.2 | 0.98x (within noise) | +1.8% |
| yardstick store-entry | ns/page | ns | 0.0725 | 0.0712 | 1.02x (within noise) | -1.8% |

Flame graphs: `page-walk/stage-1-4k-1.folded` and `stage-1-4k-32768.folded` (about 3870 samples each), with their pages. Shares by operation:

- **`4k-1`:** protect 33.7 %, unmap 39.4 %, map 22.3 %. `split_at` has no sample (it was 34.0 %).
- **`4k-32768`:** map 56.3 %, protect 19.2 %, unmap 20.0 %. Map now holds most of a range's time.

What it says:

- **Ranges: the prediction held.**
  - Protect and unmap read the floor of tables (4 for 512 pages, 67 for 32768) instead of 4 a page.
  - They run 1.93 times faster: 0.98 to 0.51 ns a page for a 512-page protect, 0.94 to 0.49 for a 32768-page unmap.
  - With 2 MiB leaves the visits fall to the floor too. The time was already under 0.01 ns a page.
- **One page: the prediction did not hold.**
  - Visits fell from 10 to 4, as predicted.
  - The time fell only 10 to 13 % at the median: protect 4.39 to 3.94 ns, unmap 4.78 to 4.17 ns. That is within the one-page noise. The minimums fell by about 0.2 ns.
  - The 34 % of samples `split_at()` held was not 34 % of the time to save. Its walks read cached tables, and an out-of-order core overlaps such loads with the work around them; a sample lands on whatever instruction is waiting when it fires.
  - What a one-page protect or unmap costs beyond a one-page map (2.56 ns) is now elsewhere in the call.
- **Map did not change**, as expected: 1.25 ns a page at 32768 pages, within the noise of the baseline's 1.17.

### Stage 2: map walks its range once, and a bulk map is one call

- **Commit:** `33a81813`. `map_range()` enters each table once and fills its entries in a loop, with a tight loop at the final level, as Zircon's `AddMapping()` and `AddMappingL0()` do. `map_frames()` runs the same walk over an array of frames, and `PageTable::map_pages_bulk()` is one call to it. `walk_range()` gets the same tight loop at the final level, as Zircon's `RemoveMappingL0()` and `UpdateMappingL0()`. The bench's bulk case maps each batch of 32 with one `map_frames()` call, as the kernel now does.
- **Decision:** the user's, 2026-10-25, after stage 1's code was shown: "try Zircon's way".
- **Predictions:**
  - The user's: none, homework is paused (2026-10-21).
  - Repair item 1 expected map to fall to about half, as protect and unmap had. Nothing was predicted for one-page calls.
- **Samples:**
  - `page-walk/stage-2.csv`, recorded as stage 1 was: three runs, 96 samples a case, the same wrappers.
  - `page-walk/stage-1-interleaved.csv` and `stage-2-interleaved.csv`: four runs of each, stage 1's bench (built from `54148303` in a worktree of its own) and stage 2's taking turns, so that both see the same load. They were taken because the one-page cases moved by more than their noise, with other builds running on the machine.

Statistics, `python3 scripts/perf_stats.py docs/perf/261025/page-walk/stage-2.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 3.62 | 3.91 | 5.36 | 5.84 | 4.30 | 0.71 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 3.91 | 4.64 | 5.67 | 6.88 | 4.74 | 0.69 |
| protect 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 4.35 | 5.13 | 6.31 | 6.59 | 5.19 | 0.73 |
| unmap 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 4K 512 | ns/page | ns | 96 | 0.69 | 0.73 | 0.94 | 1.27 | 0.80 | 0.13 |
| map 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 0.27 | 0.28 | 0.36 | 0.37 | 0.30 | 0.0404 |
| protect 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 0.14 | 0.16 | 0.21 | 0.22 | 0.18 | 0.0251 |
| unmap 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 0.67 | 0.70 | 0.91 | 1.08 | 0.76 | 0.0961 |
| map 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 0.26 | 0.27 | 0.35 | 0.35 | 0.29 | 0.0339 |
| protect 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 0.14 | 0.15 | 0.19 | 0.39 | 0.16 | 0.0302 |
| unmap 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0089 | 0.0102 | 0.0102 | 0.00825 | 0.00122 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.0051 | 0.0064 | 0.0076 | 0.0089 | 0.00679 | 0.000834 |
| protect 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0076 | 0.0089 | 0.0089 | 0.00728 | 0.00089 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0026 | 0.0051 | 0.00224 | 0.000673 |
| map 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0 | 0.0013 | 0.0025 | 0.40 | 0.00567 | 0.0411 |
| protect 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0 | 0.0013 | 0.0013 | 0.0051 | 0.00139 | 0.000556 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 0.63 | 0.67 | 0.78 | 0.98 | 0.70 | 0.0666 |
| bulk-map 4K 32 | visits/page | tables | 96 | 0.12 | 0.12 | 0.12 | 0.12 | 0.12 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 14.5 | 15.2 | 20.4 | 21.4 | 16.8 | 2.63 |
| yardstick store-entry | ns/page | ns | 96 | 0.0738 | 0.0788 | 0.0979 | 0.0992 | 0.0841 | 0.00971 |

Gain over the baseline, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/baseline.csv docs/perf/261025/page-walk/stage-2.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 2.62 | 3.91 | 0.67x (within noise) | +49.6% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 4.39 | 4.64 | 0.95x (within noise) | +5.6% |
| protect 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| unmap 4K 1 | ns/page | ns | 4.78 | 5.13 | 0.93x (within noise) | +7.3% |
| unmap 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| map 4K 512 | ns/page | ns | 1.18 | 0.73 | 1.62x | -38.5% |
| map 4K 512 | visits/page | tables | 4.00 | 0.0078 | 512.82x | -99.8% |
| protect 4K 512 | ns/page | ns | 0.98 | 0.28 | 3.45x | -71.0% |
| protect 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| unmap 4K 512 | ns/page | ns | 0.96 | 0.16 | 5.93x | -83.1% |
| unmap 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| map 4K 32768 | ns/page | ns | 1.17 | 0.70 | 1.66x | -39.8% |
| map 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.00x | -100.0% |
| protect 4K 32768 | ns/page | ns | 0.96 | 0.27 | 3.51x | -71.5% |
| protect 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| unmap 4K 32768 | ns/page | ns | 0.94 | 0.15 | 6.38x | -84.3% |
| unmap 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| map 2M 512 | ns/page | ns | 0.0051 | 0.0089 | 0.57x | +74.5% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0076 | 0.0064 | 1.19x (within noise) | -15.8% |
| protect 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| unmap 2M 512 | ns/page | ns | 0.0089 | 0.0076 | 1.17x (within noise) | -14.6% |
| unmap 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0059 | 0.0001 | 59.00x | -98.3% |
| protect 2M 32768 | ns/page | ns | 0.0025 | 0.0013 | 1.92x (within noise) | -48.0% |
| protect 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| unmap 2M 32768 | ns/page | ns | 0.0025 | 0.0013 | 1.92x | -48.0% |
| unmap 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| bulk-map 4K 32 | ns/page | ns | 3.27 | 0.67 | 4.92x | -79.7% |
| bulk-map 4K 32 | visits/page | tables | 4.00 | 0.12 | 32.00x | -96.9% |
| yardstick zero-page | ns/page | ns | 15.0 | 15.2 | 0.98x (within noise) | +1.6% |
| yardstick store-entry | ns/page | ns | 0.0725 | 0.0788 | 0.92x (within noise) | +8.7% |

Gain over stage 1, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-1.csv docs/perf/261025/page-walk/stage-2.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 2.56 | 3.91 | 0.65x (within noise) | +52.9% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 3.94 | 4.64 | 0.85x (within noise) | +17.8% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 4.17 | 5.13 | 0.81x (within noise) | +23.1% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 1.27 | 0.73 | 1.76x | -43.1% |
| map 4K 512 | visits/page | tables | 4.00 | 0.0078 | 512.82x | -99.8% |
| protect 4K 512 | ns/page | ns | 0.51 | 0.28 | 1.79x | -44.1% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 0.49 | 0.16 | 3.06x | -67.3% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 1.25 | 0.70 | 1.78x | -43.9% |
| map 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.00x | -100.0% |
| protect 4K 32768 | ns/page | ns | 0.50 | 0.27 | 1.83x | -45.3% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.49 | 0.15 | 3.29x | -69.6% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0051 | 0.0089 | 0.57x | +74.5% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0076 | 0.0064 | 1.19x (within noise) | -15.8% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0076 | 0.0076 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0059 | 0.0001 | 59.00x | -98.3% |
| protect 2M 32768 | ns/page | ns | 0.0013 | 0.0013 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0013 | 0.0013 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 3.49 | 0.67 | 5.25x | -81.0% |
| bulk-map 4K 32 | visits/page | tables | 4.00 | 0.12 | 32.00x | -96.9% |
| yardstick zero-page | ns/page | ns | 15.2 | 15.2 | 1.00x (within noise) | -0.1% |
| yardstick store-entry | ns/page | ns | 0.0712 | 0.0788 | 0.90x (within noise) | +10.7% |

The same two stages taking turns, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-1-interleaved.csv docs/perf/261025/page-walk/stage-2-interleaved.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 2.55 | 3.96 | 0.64x | +55.3% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 3.88 | 4.43 | 0.88x (within noise) | +14.2% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 4.31 | 4.89 | 0.88x (within noise) | +13.4% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 1.27 | 0.75 | 1.71x | -41.4% |
| map 4K 512 | visits/page | tables | 4.00 | 0.0078 | 512.82x | -99.8% |
| protect 4K 512 | ns/page | ns | 0.50 | 0.29 | 1.76x | -43.3% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 0.49 | 0.16 | 3.19x | -68.6% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 1.26 | 0.72 | 1.74x | -42.4% |
| map 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.00x | -100.0% |
| protect 4K 32768 | ns/page | ns | 0.49 | 0.28 | 1.76x | -43.2% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.49 | 0.16 | 3.13x | -68.0% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0051 | 0.0089 | 0.57x | +74.5% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0063 | 0.0064 | 0.98x (within noise) | +1.6% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0076 | 0.0076 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0025 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0059 | 0.0001 | 59.00x | -98.3% |
| protect 2M 32768 | ns/page | ns | 0.0013 | 0.0013 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0013 | 0.0013 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 3.57 | 0.67 | 5.34x | -81.3% |
| bulk-map 4K 32 | visits/page | tables | 4.00 | 0.12 | 32.00x | -96.9% |
| yardstick zero-page | ns/page | ns | 16.2 | 16.1 | 1.00x (within noise) | -0.4% |
| yardstick store-entry | ns/page | ns | 0.0763 | 0.0814 | 0.94x (within noise) | +6.7% |

Instructions one call runs, counted with lldb one instruction at a time, in the bench's steady state (the 21st call of `spin 1 4k-1`, its tables already there):

| call | stage 1 | stage 2 |
|---|---|---|
| map, 1 page | 90 | 125 |
| protect, 1 page | 134 | 141 |
| unmap, 1 page | 138 | 152 |

Flame graphs: `page-walk/stage-2-4k-1.folded`, `stage-2-4k-32768.folded` and `stage-2-bulk-32.folded` (about 3350 samples each), with their pages. Shares by operation:

- **`4k-1`:** unmap 37.0 %, protect 30.3 %, map 28.5 %.
- **`4k-32768`:** map 57.7 %, protect 21.7 %, unmap 13.2 %, clearing the arena between samples 6.3 %.
- **`bulk-32`:** `map_frames()` 90.1 %, clearing the arena 8.2 %. The engine is now one frame under the bench, with no per-page call (96.5 % was the per-page call in the baseline).

What it says:

- **Ranges: the prediction held, and more.**
  - A map reads the floor of tables, and runs 1.78 times faster than stage 1 at 32768 pages: 1.25 to 0.70 ns a page. That is 0.56 of stage 1, close to the half item 1 expected.
  - A bulk map of 32 scattered frames runs 5.25 times faster: 3.49 to 0.67 ns a page, as cheap as a contiguous map. Most of its cost was the call per page, as the baseline's inlined run had shown.
  - The final-level loop took protect from 0.50 to 0.27 ns a page and unmap from 0.49 to 0.15 at 32768 pages, which no one predicted: stage 1 already read the floor of tables, and what was left was the checks for a part of a leaf and for a table, made on every entry of the final level, where neither can be.
  - Against the baseline, a 32768-page range costs 0.60 (map), 0.28 (protect) and 0.16 (unmap) of what it did, and a bulk map 0.20.
- **One page: map is slower, beyond the noise.**
  - Taking turns with stage 1, a one-page map costs 3.96 ns at the median instead of 2.55, 55 % more. It runs 125 instructions a call instead of 90.
  - The extra instructions are the generality of a range walk: leaving a loop at each of the four levels after the leaf is written, the frame cursor's check for the next frame, and three more pairs of registers saved and restored.
  - One-page protect and unmap cost about 0.5 ns more (14 %, inside their noise), with 7 and 14 more instructions.
  - A page fault maps one page, so it pays this. It is about 1.4 ns, next to the 15 ns of zeroing the page the fault maps. It is repair item 1 now, with its candidates, for the user to decide.

Code size of stage 2 in the kernel, the numbers the CRTP rewrite (stage 3) is compared with: `page-walk/stage-2-code-size.csv`, with the command in its header. `python3 scripts/perf_stats.py docs/perf/261025/page-walk/stage-2-code-size.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map | code bytes | bytes | 1 | 8626 | 8626 | 8626 | 8626 | 8626 | 0 |
| map | functions | functions | 1 | 20.0 | 20.0 | 20.0 | 20.0 | 20.0 | 0 |
| map | indirect | calls | 1 | 23.0 | 23.0 | 23.0 | 23.0 | 23.0 | 0 |
| map | external | callees | 1 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| map_bulk | code bytes | bytes | 1 | 8692 | 8692 | 8692 | 8692 | 8692 | 0 |
| map_bulk | functions | functions | 1 | 20.0 | 20.0 | 20.0 | 20.0 | 20.0 | 0 |
| map_bulk | indirect | calls | 1 | 23.0 | 23.0 | 23.0 | 23.0 | 23.0 | 0 |
| map_bulk | external | callees | 1 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| unmap | code bytes | bytes | 1 | 3232 | 3232 | 3232 | 3232 | 3232 | 0 |
| unmap | functions | functions | 1 | 5.00 | 5.00 | 5.00 | 5.00 | 5.00 | 0 |
| unmap | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| unmap | external | callees | 1 | 8.00 | 8.00 | 8.00 | 8.00 | 8.00 | 0 |
| protect | code bytes | bytes | 1 | 3129 | 3129 | 3129 | 3129 | 3129 | 0 |
| protect | functions | functions | 1 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| protect | external | callees | 1 | 7.00 | 7.00 | 7.00 | 7.00 | 7.00 | 0 |
| query | code bytes | bytes | 1 | 481.0 | 481.0 | 481.0 | 481.0 | 481.0 | 0 |
| query | functions | functions | 1 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0 |
| query | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| query | external | callees | 1 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0 |

What it says:

- **A map may run through 8.6 KB in 20 functions.** The engine is instantiated twice, once for the kernel's tables (`NoDeposits`) and once for a user address space's (`PageTableDeposits`), and `PageTable::map_pages()` picks one at run time, so both are in its closure: 2.9 KB of the first kind, 4.8 KB of the second with its deposit functions, 0.8 KB shared. One kind of table reaches 5.7 KB of it.
- **The rest of a map's closure is undo and deposits:** `walk_range()` (Undo unmaps the prefix) and `reserve()`, `withdraw()`, `discard()`.
- **A map makes 23 indirect calls in its closure; unmap, protect and query none.** `PageTable` passes `phys_to_virt` and `alloc_table` to the engine as functions, and the engine's map is out of line, so each `map_range()` calls them through a pointer: `callq *%rdx` to enter each table, `callq *-0x38(%rbp)` to allocate one. Unmap and protect inline the engine into `PageTable`, where the function is known and the call is direct. The host bench passes a functor, which is always inlined, so it does not see these calls: a one-page map in the kernel enters four tables through four indirect calls the bench does not make.
- **Unmap and protect are 3.1 to 3.2 KB in 4 or 5 functions**, the engine inlined into `PageTable::unmap_pages()` (1917 bytes) and `protect_pages()` (2395 bytes).

### Stage 3: x86 walks its own tables through a CRTP base

- **Commits:** `f4dc7e20`, `46c4ddf1`, `588d02df`, `172cad3f` move query, unmap and protect, map and map_frames, and the rest into `page_tables::X86PageTableImpl<Derived>` (`kernel/arch/x86/lib/page_tables`), whose walks take the level as an argument and ask the derived class at run time whether a table splits its large leaves (`.agents/tasks/x86-page-table-crtp`). `09c6a5d6` moves the bench onto it.
- **Decision:** the user's, 2026-10-25 and 26: x86 walks its own tables through a CRTP base, as the first page table did (D1, option A), in a library of its own (D2, (b)), with one cursor for both maps (D3, (a)).
- **Predictions:**
  - The user's reasons: the engine is hard to control, and it generates code twice for nothing, which does not help the cache.
  - Mine, before the bench moved: code down; ranges about as fast, since the loops have the same shape; one-page calls slower, since each level is now a call.
- **Samples:**
  - `page-walk/stage-3.csv`: three runs, 96 samples a case, recorded as stage 2 was.
  - `page-walk/stage-2-beside-3.csv` and `stage-3-beside-2.csv`: four runs of each, stage 2's bench (built from `ba95bbba`) and stage 3's taking turns.
  - `page-walk/stage-3-code-size.csv`: the kernel's code, as `stage-2-code-size.csv` was measured, with the base's names added to `--within`.

Statistics, `python3 scripts/perf_stats.py docs/perf/261025/page-walk/stage-3.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 9.96 | 11.3 | 14.7 | 18.1 | 11.9 | 1.68 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 8.65 | 9.96 | 12.3 | 20.8 | 10.6 | 2.22 |
| protect 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 8.59 | 9.74 | 11.4 | 22.1 | 10.3 | 1.89 |
| unmap 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 4K 512 | ns/page | ns | 96 | 1.63 | 1.78 | 2.15 | 5.94 | 1.96 | 0.64 |
| map 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 0.64 | 0.70 | 1.04 | 6.49 | 0.87 | 0.67 |
| protect 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 0.46 | 0.52 | 2.99 | 3.29 | 1.03 | 0.96 |
| unmap 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 1.60 | 1.67 | 1.93 | 6.35 | 1.78 | 0.52 |
| map 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 0.62 | 0.64 | 0.81 | 2.87 | 0.70 | 0.25 |
| protect 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 0.45 | 0.47 | 1.11 | 4.77 | 0.71 | 0.52 |
| unmap 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0165 | 0.0178 | 0.0191 | 0.30 | 0.0211 | 0.0284 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.014 | 0.0153 | 0.0165 | 0.28 | 0.0184 | 0.027 |
| protect 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.014 | 0.0153 | 0.0165 | 0.0204 | 0.0156 | 0.00109 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0038 | 0.0038 | 0.0051 | 0.0076 | 0.00422 | 0.000735 |
| map 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0.0025 | 0.0038 | 0.00445 | 0.0051 | 0.00383 | 0.000556 |
| protect 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0.0025 | 0.0038 | 0.0051 | 0.0076 | 0.00393 | 0.000753 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 1.77 | 1.93 | 2.33 | 8.18 | 2.10 | 0.75 |
| bulk-map 4K 32 | visits/page | tables | 96 | 0.12 | 0.12 | 0.12 | 0.12 | 0.12 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 14.5 | 15.8 | 18.9 | 52.2 | 17.2 | 5.68 |
| yardstick store-entry | ns/page | ns | 96 | 0.0725 | 0.0814 | 0.0979 | 0.36 | 0.0882 | 0.0357 |

Gain over the baseline, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/baseline.csv docs/perf/261025/page-walk/stage-3.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 2.62 | 11.3 | 0.23x | +330.0% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 4.39 | 9.96 | 0.44x | +126.7% |
| protect 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| unmap 4K 1 | ns/page | ns | 4.78 | 9.74 | 0.49x | +103.6% |
| unmap 4K 1 | visits/page | tables | 10.0 | 4.00 | 2.50x | -60.0% |
| map 4K 512 | ns/page | ns | 1.18 | 1.78 | 0.66x | +51.0% |
| map 4K 512 | visits/page | tables | 4.00 | 0.0078 | 512.82x | -99.8% |
| protect 4K 512 | ns/page | ns | 0.98 | 0.70 | 1.41x (within noise) | -28.9% |
| protect 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| unmap 4K 512 | ns/page | ns | 0.96 | 0.52 | 1.85x (within noise) | -45.8% |
| unmap 4K 512 | visits/page | tables | 4.01 | 0.0078 | 514.32x | -99.8% |
| map 4K 32768 | ns/page | ns | 1.17 | 1.67 | 0.70x | +43.3% |
| map 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.00x | -100.0% |
| protect 4K 32768 | ns/page | ns | 0.96 | 0.64 | 1.51x | -33.8% |
| protect 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| unmap 4K 32768 | ns/page | ns | 0.94 | 0.47 | 2.01x (within noise) | -50.1% |
| unmap 4K 32768 | visits/page | tables | 4.00 | 0.002 | 2000.10x | -100.0% |
| map 2M 512 | ns/page | ns | 0.0051 | 0.0178 | 0.29x | +249.0% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0076 | 0.0153 | 0.50x | +101.3% |
| protect 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| unmap 2M 512 | ns/page | ns | 0.0089 | 0.0153 | 0.58x | +71.9% |
| unmap 2M 512 | visits/page | tables | 0.0176 | 0.0059 | 2.98x | -66.5% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0038 | 0.66x (within noise) | +52.0% |
| map 2M 32768 | visits/page | tables | 0.0059 | 0.0001 | 59.00x | -98.3% |
| protect 2M 32768 | ns/page | ns | 0.0025 | 0.0038 | 0.66x | +52.0% |
| protect 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| unmap 2M 32768 | ns/page | ns | 0.0025 | 0.0038 | 0.66x (within noise) | +52.0% |
| unmap 2M 32768 | visits/page | tables | 0.006 | 0.0001 | 60.00x | -98.3% |
| bulk-map 4K 32 | ns/page | ns | 3.27 | 1.93 | 1.70x | -41.0% |
| bulk-map 4K 32 | visits/page | tables | 4.00 | 0.12 | 32.00x | -96.9% |
| yardstick zero-page | ns/page | ns | 15.0 | 15.8 | 0.95x (within noise) | +5.3% |
| yardstick store-entry | ns/page | ns | 0.0725 | 0.0814 | 0.89x (within noise) | +12.3% |

Gain over stage 2, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-2.csv docs/perf/261025/page-walk/stage-3.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 3.91 | 11.3 | 0.35x | +187.5% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 4.64 | 9.96 | 0.47x | +114.7% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 5.13 | 9.74 | 0.53x | +89.8% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 0.73 | 1.78 | 0.41x | +145.3% |
| map 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| protect 4K 512 | ns/page | ns | 0.28 | 0.70 | 0.41x | +145.3% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 0.16 | 0.52 | 0.31x (within noise) | +221.2% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 0.70 | 1.67 | 0.42x | +138.1% |
| map 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| protect 4K 32768 | ns/page | ns | 0.27 | 0.64 | 0.43x | +132.1% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.15 | 0.47 | 0.31x (within noise) | +218.1% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0089 | 0.0178 | 0.50x | +100.0% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0064 | 0.0153 | 0.42x | +139.1% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0076 | 0.0153 | 0.50x | +101.3% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0038 | 0.66x (within noise) | +52.0% |
| map 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0013 | 0.0038 | 0.34x | +192.3% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0013 | 0.0038 | 0.34x | +192.3% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 0.67 | 1.93 | 0.34x | +190.1% |
| bulk-map 4K 32 | visits/page | tables | 0.12 | 0.12 | 1.00x (within noise) | +0.0% |
| yardstick zero-page | ns/page | ns | 15.2 | 15.8 | 0.97x (within noise) | +3.6% |
| yardstick store-entry | ns/page | ns | 0.0788 | 0.0814 | 0.97x (within noise) | +3.3% |

The same two stages taking turns, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-2-beside-3.csv docs/perf/261025/page-walk/stage-3-beside-2.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 3.79 | 10.7 | 0.35x | +183.4% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 4.35 | 9.35 | 0.47x | +115.0% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 4.63 | 9.22 | 0.50x | +98.9% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 0.74 | 1.65 | 0.45x | +122.1% |
| map 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| protect 4K 512 | ns/page | ns | 0.28 | 0.69 | 0.41x | +144.1% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 0.15 | 0.47 | 0.32x | +211.1% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 0.72 | 1.61 | 0.45x | +123.8% |
| map 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| protect 4K 32768 | ns/page | ns | 0.30 | 0.67 | 0.44x | +126.0% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.15 | 0.45 | 0.32x | +211.7% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0076 | 0.0191 | 0.40x | +151.3% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0063 | 0.0153 | 0.41x | +142.9% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0064 | 0.0153 | 0.42x | +139.1% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0025 | 0.0038 | 0.66x (within noise) | +52.0% |
| map 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0013 | 0.0038 | 0.34x | +192.3% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0013 | 0.0038 | 0.34x | +192.3% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 0.71 | 1.83 | 0.39x | +157.9% |
| bulk-map 4K 32 | visits/page | tables | 0.12 | 0.12 | 1.00x (within noise) | +0.0% |
| yardstick zero-page | ns/page | ns | 15.8 | 14.7 | 1.08x (within noise) | -7.2% |
| yardstick store-entry | ns/page | ns | 0.0789 | 0.0763 | 1.03x (within noise) | -3.3% |

Code size in the kernel, `python3 scripts/perf_stats.py docs/perf/261025/page-walk/stage-3-code-size.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map | code bytes | bytes | 1 | 4121 | 4121 | 4121 | 4121 | 4121 | 0 |
| map | functions | functions | 1 | 8.00 | 8.00 | 8.00 | 8.00 | 8.00 | 0 |
| map | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| map | external | callees | 1 | 9.00 | 9.00 | 9.00 | 9.00 | 9.00 | 0 |
| map_bulk | code bytes | bytes | 1 | 4137 | 4137 | 4137 | 4137 | 4137 | 0 |
| map_bulk | functions | functions | 1 | 9.00 | 9.00 | 9.00 | 9.00 | 9.00 | 0 |
| map_bulk | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| map_bulk | external | callees | 1 | 9.00 | 9.00 | 9.00 | 9.00 | 9.00 | 0 |
| unmap | code bytes | bytes | 1 | 2183 | 2183 | 2183 | 2183 | 2183 | 0 |
| unmap | functions | functions | 1 | 6.00 | 6.00 | 6.00 | 6.00 | 6.00 | 0 |
| unmap | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| unmap | external | callees | 1 | 8.00 | 8.00 | 8.00 | 8.00 | 8.00 | 0 |
| protect | code bytes | bytes | 1 | 1665 | 1665 | 1665 | 1665 | 1665 | 0 |
| protect | functions | functions | 1 | 5.00 | 5.00 | 5.00 | 5.00 | 5.00 | 0 |
| protect | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| protect | external | callees | 1 | 7.00 | 7.00 | 7.00 | 7.00 | 7.00 | 0 |
| query | code bytes | bytes | 1 | 423.0 | 423.0 | 423.0 | 423.0 | 423.0 | 0 |
| query | functions | functions | 1 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0 |
| query | indirect | calls | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| query | external | callees | 1 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0 |

Against stage 2, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-2-code-size.csv docs/perf/261025/page-walk/stage-3-code-size.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map | code bytes | bytes | 8626 | 4121 | 2.09x | -52.2% |
| map | functions | functions | 20.0 | 8.00 | 2.50x | -60.0% |
| map | indirect | calls | 23.0 | 0 | - | - |
| map | external | callees | 10.0 | 9.00 | 1.11x | -10.0% |
| map_bulk | code bytes | bytes | 8692 | 4137 | 2.10x | -52.4% |
| map_bulk | functions | functions | 20.0 | 9.00 | 2.22x | -55.0% |
| map_bulk | indirect | calls | 23.0 | 0 | - | - |
| map_bulk | external | callees | 10.0 | 9.00 | 1.11x | -10.0% |
| unmap | code bytes | bytes | 3232 | 2183 | 1.48x | -32.5% |
| unmap | functions | functions | 5.00 | 6.00 | 0.83x | +20.0% |
| unmap | indirect | calls | 0 | 0 | - | - |
| unmap | external | callees | 8.00 | 8.00 | 1.00x (within noise) | +0.0% |
| protect | code bytes | bytes | 3129 | 1665 | 1.88x | -46.8% |
| protect | functions | functions | 4.00 | 5.00 | 0.80x | +25.0% |
| protect | indirect | calls | 0 | 0 | - | - |
| protect | external | callees | 7.00 | 7.00 | 1.00x (within noise) | +0.0% |
| query | code bytes | bytes | 481.0 | 423.0 | 1.14x | -12.1% |
| query | functions | functions | 2.00 | 2.00 | 1.00x (within noise) | +0.0% |
| query | indirect | calls | 0 | 0 | - | - |
| query | external | callees | 2.00 | 2.00 | 1.00x (within noise) | +0.0% |

Instructions one call runs, counted with lldb as for stage 2 (the 21st call of `spin 1 4k-1`):

| call | stage 1 | stage 2 | stage 3 |
|---|---|---|---|
| map, 1 page | 90 | 125 | 357 |
| protect, 1 page | 134 | 141 | 337 |
| unmap, 1 page | 138 | 152 | 315 |

Flame graphs: `page-walk/stage-3-4k-1.folded`, `stage-3-4k-32768.folded` and `stage-3-bulk-32.folded` (about 3350 samples each), with their pages. Shares by operation: `4k-1` map 37.4 %, protect 32.8 %, unmap 29.0 %; `4k-32768` map 52.5 %, unmap 23.0 %, protect 21.3 %; `bulk-32` `map_frames()` 94.2 %. Each walk is a frame of its own now (`map_range`, `walk_range`), where the engine's was inlined into its wrapper.

What it says:

- **Code: the user's reason held.** A map may run through 4121 bytes instead of 8626, in 8 functions instead of 20, with no indirect call instead of 23; unmap 2183 bytes instead of 3232, protect 1665 instead of 3129. Whether that saves instruction-cache misses is not measured: it needs a hardware PMU.
- **Time: my prediction failed for ranges, and every case is slower.** Taking turns with stage 2, a 32768-page map costs 1.61 ns a page instead of 0.72, an unmap 0.45 instead of 0.15, a bulk map 1.83 instead of 0.71; one-page calls cost 9 to 11 ns instead of 4 to 5.
- **Why, from the disassembly of the bench's `map_range()`:**
  - **Aliasing.** A table's entries are `PteVal`, an `unsigned long`, the type of every field of the cursors. The compiler must assume a store to an entry may change the cursor, so after each entry the last-level loop loads the cursor's address, bytes left and run back from memory and stores them again. The engine's entries were `X86Pte`, a struct, which type-based alias analysis keeps apart from the cursor's fields, and its loop kept the cursor in registers. This is most of what the ranges lost.
  - **Constants for a split, made on every call.** `split()` is inlined into `map_range()` and `walk_range()`, and its 512-entry fill was vectorised; the vector constants it needs are made at the top of every call of those functions, about 30 instructions, whether or not anything splits. With one call per level, a one-page call pays them four times.
  - **A call per level.** The level is an argument, so each level above the last is a call of the walk, which saves and restores six pairs of registers; the engine's levels were templates, inlined into one function.
- **Two exploratory runs** (the bench's medians, one run each, not a stage; the code was put back after): with `split()` kept out of line, a one-page map ran 294 instructions and 8.9 ns, and the ranges did not change; with the cursor also copied into locals in the two last-level loops, a 32768-page map ran 0.78 ns a page and an unmap 0.17, close to stage 2, while a protect stayed at 0.72 and one-page calls at 6.7 to 8.1 ns. These are the candidates put to the user for stage 4.

#### Beside a mirror of Zircon's walks

The user asked, after seeing stage 3, to copy Zircon's walks as they are and measure them. `benches/kernel/lib/arch/mirror_walks.hpp` rewrites them function for function (not their text, which would carry Zircon's licence header): `AddMapping()` and `AddMappingL0()`, `RemoveMapping()` and `RemoveMappingL0()`, `UpdateMapping()` and `UpdateMappingL0()`, `SplitLargePage()`, `MappingCursor` and `VirtualAddressCursor`, entries through `volatile` pointers, `UpdateEntry()` and `UnmapEntry()` queueing each changed address in a 32-item pending-invalidation list, a count of present entries per table, and unmap freeing a table whose count falls to zero (`kernel/arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`, `kernel/vm/include/vm/mapping_cursor.h`). Left out: the lock, the hardware invalidation and cache flushes, and the page-descriptor lookup, an array index here. `page-walk-bench-mirror` is the same bench driving it, with the same cases and names, and its `check` mode reads every page back through this kernel's `query()`; it passes, and fails when the mirror drops the write bit.

One trap, found in the first run: the mirror's entries were `u64`. On the host `uint64_t` is `unsigned long long` while `size_t` is `unsigned long`, so its entries and its cursors' fields were of different types, and the compiler kept the cursors in registers. On the x86-64 kernel both are `unsigned long`, as Zircon's `pt_entry_t` and `vaddr_t` are. The run is kept as `page-walk/mirror-u64.csv` (with `mirror-u64-beside-3.csv` and `stage-3-beside-mirror-u64.csv`), because it is the cause of stage 3's lost range time shown the other way round; the mirror's entries are `usize` since.

Samples: `page-walk/mirror.csv`, three runs; `page-walk/stage-3-beside-mirror.csv` and `mirror-beside-3.csv`, four runs of each taking turns.

Statistics, `python3 scripts/perf_stats.py docs/perf/261025/page-walk/mirror.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 9.34 | 10.4 | 11.0 | 16.8 | 10.4 | 0.83 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 7.17 | 7.72 | 8.08 | 9.12 | 7.69 | 0.33 |
| protect 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 8.71 | 9.49 | 9.82 | 11.2 | 9.46 | 0.43 |
| unmap 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 4K 512 | ns/page | ns | 96 | 1.63 | 1.76 | 1.88 | 2.24 | 1.76 | 0.11 |
| map 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 1.55 | 1.66 | 1.77 | 2.03 | 1.66 | 0.0991 |
| protect 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 1.54 | 1.65 | 1.77 | 1.99 | 1.65 | 0.1 |
| unmap 4K 512 | visits/page | tables | 96 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0.0078 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 1.60 | 1.72 | 1.90 | 2.19 | 1.74 | 0.13 |
| map 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 1.54 | 1.66 | 1.89 | 2.15 | 1.68 | 0.13 |
| protect 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 1.53 | 1.65 | 1.79 | 2.66 | 1.67 | 0.15 |
| unmap 4K 32768 | visits/page | tables | 96 | 0.002 | 0.002 | 0.002 | 0.002 | 0.002 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0165 | 0.0178 | 0.0216 | 0.0229 | 0.0182 | 0.00209 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.0102 | 0.0127 | 0.0165 | 0.0216 | 0.0132 | 0.00192 |
| protect 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.014 | 0.0165 | 0.0204 | 0.0814 | 0.0175 | 0.00692 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0038 | 0.00445 | 0.0063 | 0.0064 | 0.0046 | 0.000889 |
| map 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0.0038 | 0.0038 | 0.0051 | 0.0063 | 0.00415 | 0.000607 |
| protect 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0.0038 | 0.0051 | 0.0064 | 0.0089 | 0.00565 | 0.00102 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0.0001 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 1.70 | 1.77 | 2.38 | 2.73 | 1.98 | 0.30 |
| bulk-map 4K 32 | visits/page | tables | 96 | 0.12 | 0.12 | 0.12 | 0.12 | 0.12 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 14.6 | 15.3 | 19.6 | 20.6 | 16.0 | 1.79 |
| yardstick store-entry | ns/page | ns | 96 | 0.0738 | 0.0788 | 0.10 | 0.18 | 0.0838 | 0.014 |

The mirror against stage 3, taking turns, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-3-beside-mirror.csv docs/perf/261025/page-walk/mirror-beside-3.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 10.8 | 10.2 | 1.06x (within noise) | -5.8% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 9.43 | 7.57 | 1.24x | -19.7% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 9.34 | 9.36 | 1.00x (within noise) | +0.2% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 1.68 | 1.67 | 1.00x (within noise) | -0.2% |
| map 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| protect 4K 512 | ns/page | ns | 0.68 | 1.58 | 0.43x | +131.7% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 1.01 | 1.57 | 0.64x | +56.1% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 1.64 | 1.63 | 1.01x (within noise) | -0.7% |
| map 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| protect 4K 32768 | ns/page | ns | 0.68 | 1.57 | 0.43x | +131.3% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.47 | 1.56 | 0.30x | +235.2% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0191 | 0.0165 | 1.16x (within noise) | -13.6% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0165 | 0.0127 | 1.30x (within noise) | -23.0% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0165 | 0.0165 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0051 | 0.0051 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0038 | 0.0038 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0038 | 0.0051 | 0.75x (within noise) | +34.2% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 1.89 | 1.83 | 1.04x (within noise) | -3.4% |
| bulk-map 4K 32 | visits/page | tables | 0.12 | 0.12 | 1.00x (within noise) | +0.0% |
| yardstick zero-page | ns/page | ns | 16.0 | 14.9 | 1.07x (within noise) | -6.7% |
| yardstick store-entry | ns/page | ns | 0.0814 | 0.0776 | 1.05x (within noise) | -4.7% |

The first run, entries `unsigned long long`, `python3 scripts/perf_stats.py --compare docs/perf/261025/page-walk/stage-3-beside-mirror-u64.csv docs/perf/261025/page-walk/mirror-u64-beside-3.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 10.3 | 9.58 | 1.08x (within noise) | -7.1% |
| map 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| protect 4K 1 | ns/page | ns | 8.92 | 7.47 | 1.19x | -16.2% |
| protect 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| unmap 4K 1 | ns/page | ns | 9.07 | 8.72 | 1.04x (within noise) | -3.8% |
| unmap 4K 1 | visits/page | tables | 4.00 | 4.00 | 1.00x (within noise) | +0.0% |
| map 4K 512 | ns/page | ns | 1.65 | 0.69 | 2.37x | -57.8% |
| map 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| protect 4K 512 | ns/page | ns | 0.64 | 0.64 | 1.01x (within noise) | -1.0% |
| protect 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| unmap 4K 512 | ns/page | ns | 0.99 | 0.68 | 1.47x (within noise) | -31.8% |
| unmap 4K 512 | visits/page | tables | 0.0078 | 0.0078 | 1.00x (within noise) | +0.0% |
| map 4K 32768 | ns/page | ns | 1.61 | 0.67 | 2.41x | -58.5% |
| map 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| protect 4K 32768 | ns/page | ns | 0.63 | 0.62 | 1.02x (within noise) | -2.4% |
| protect 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| unmap 4K 32768 | ns/page | ns | 0.46 | 0.66 | 0.70x | +42.9% |
| unmap 4K 32768 | visits/page | tables | 0.002 | 0.002 | 1.00x (within noise) | +0.0% |
| map 2M 512 | ns/page | ns | 0.0191 | 0.0153 | 1.25x (within noise) | -19.9% |
| map 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| protect 2M 512 | ns/page | ns | 0.0165 | 0.0127 | 1.30x (within noise) | -23.0% |
| protect 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | ns/page | ns | 0.0153 | 0.0153 | 1.00x (within noise) | +0.0% |
| unmap 2M 512 | visits/page | tables | 0.0059 | 0.0059 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | ns/page | ns | 0.0038 | 0.0038 | 1.00x (within noise) | +0.0% |
| map 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | ns/page | ns | 0.0038 | 0.0038 | 1.00x (within noise) | +0.0% |
| protect 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | ns/page | ns | 0.0038 | 0.0038 | 1.00x (within noise) | +0.0% |
| unmap 2M 32768 | visits/page | tables | 0.0001 | 0.0001 | 1.00x (within noise) | +0.0% |
| bulk-map 4K 32 | ns/page | ns | 1.79 | 1.24 | 1.45x | -30.9% |
| bulk-map 4K 32 | visits/page | tables | 0.12 | 0.12 | 1.00x (within noise) | +0.0% |
| yardstick zero-page | ns/page | ns | 14.8 | 14.6 | 1.01x (within noise) | -0.8% |
| yardstick store-entry | ns/page | ns | 0.0776 | 0.0763 | 1.02x (within noise) | -1.7% |

Instructions one call runs (lldb, as above): map 369, protect 268, unmap 338, against stage 3's 357, 337, 315.

What it says:

- **Zircon's walks, as they would run in the kernel, are no faster than stage 3.** A range map costs the same (1.63 and 1.64 ns a page): the same aliasing between entries and cursors. A range protect or unmap costs more (about 1.57 ns a page against 0.68 and 0.47): every entry changed is also queued for invalidation, with its own stores. One-page calls are within a few nanoseconds of stage 3's, both far from stage 2's.
- **The first run is fix A, measured by accident.** The same code with entries of a type apart from the cursors maps a range at 0.67 ns a page instead of 1.63, 2.4 times faster, stage 2's time. A struct for the entry (fix A) gives the kernel what `unsigned long long` gave the host.

## Conclusion
Where it stands after stage 3:

- **Ranges and bulk maps reached the floor of tables in stage 2** (0.15 to 0.73 ns a page, from 0.94 to 1.18 in the baseline) and keep it in stage 3, but stage 3's code is two to three times slower per page.
- **Stage 3 halved the code of a map and removed its indirect calls**, as the user expected, and gave x86 its own walks; it lost time to aliasing between entries and cursors, to the split's constants made on every call, and to a call per level. The first two are not inherent to the CRTP design, and the exploratory runs suggest fixing them brings ranges back to stage 2's time; the third is the price of the level as an argument, and what to do about it is a decision for the user.
- **Copying Zircon's walks would not have helped:** run with the kernel's types, they are as slow as stage 3 on a range map and slower on protect and unmap; their first host run, whose entries happened to be a type apart, is the measurement that fix A works.
- **One-page calls** remain repair item 1 of `docs/repairs/261025/arch-paging.md`, now with stage 3's numbers.
- **What is left outside the walk** is the invalidation, the shootdown and the lock (repair items 2 to 5), which need the kernel and, for their time, hardware.
