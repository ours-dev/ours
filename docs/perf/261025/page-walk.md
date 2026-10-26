# page-walk: performance report

**Date**: 2026-10-25
**Status**: Measuring

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
None

## Conclusion
None: measuring. Which candidate to take first, if any, and whether to measure the kernel's costs around the engine (items 3 to 6) first, is the user's to decide.
