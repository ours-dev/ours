# page-walk: performance report

**Date**: 2026-10-25
**Status**: Measuring

## Question
Whether the paging engine's walks are worth making faster. Repair items 1 and 2 of `docs/repairs/261025/arch-paging.md` say:

- **Item 1:** every leaf of a range is found by a walk from the root.
- **Item 2:** a bulk map is one engine call per page.

The user expected a large room for optimisation there, and asked for the data first (2026-10-25).

## Method
`page-walk-bench` (`benches/kernel/lib/arch/page_walk_bench.cpp`) calls the engine, `arch::Paging` with x86's four levels, directly on the host. It uses page tables in an arena whose addresses stand in for physical ones. The engine is header-only, and its walks are software on ordinary memory, with no TLB in them, so the host's time is a fair number for that work. It leaves out what the kernel adds around them: the lock, interrupts off, and the shootdown.

- **Machine and build:** Apple M5 Pro, macOS 27.0.1, clang 23.1.0-rc3, host toolchain of `out/bringup.x86`.
- **Command:** `out/bringup.x86/host/obj/benches/kernel/lib/arch/page-walk-bench csv`. It was run three times, and the samples of the three runs are kept together in `page-walk/baseline.csv`.
- **Samples:** 32 a case in each run, 96 in all.
  - Each sample starts from empty tables and covers 32768 pages: it maps, protects and unmaps `32768 / n` ranges of `n` pages one after another, each phase timed as a whole.
  - The host clock ticks every 41.7 ns, longer than one operation on one page, so a sample has to cover many.
  - A full untimed pass comes first. Without it the first run was up to 60 % slower: a process just started may run on a slower core or clock.
- **Cases:**
  - 4 KiB leaves, with frames a page off a 2 MiB boundary, for 1, 512 and 32768 pages;
  - 2 MiB leaves, with aligned frames and deposits always granted, for 512 and 32768 pages;
  - a 32-frame bulk map done as `PageTable::map_pages_bulk()` does it, one engine call a page.
- **Metrics:**
  - `visits/page`: the tables the engine enters per page, counted exactly by the `PhysToVirt` it is given. The same on any machine.
  - `ns/page`: host time per page.
  - Two yardsticks: zeroing a 4 KiB page (1024 distinct pages a sample), and storing an entry with no walk (32768 a sample).
- **The floor:** the distinct tables a range touches, which a walk going down once per table would read. 4 for one page; 4 for 512 pages; 67 for 32768 pages at 4 KiB; 3 for 2 MiB leaves. The bench prints it beside its summary.
- **Cache:** a sample's 4 KiB tables hold 256 KiB of entries, more than L1. A single 512-page range on its own, in L1, measured about the same in the first exploratory runs, 1.2 ns a page.

## Baseline
`page-walk/baseline.csv`, statistics by `python3 scripts/perf_stats.py docs/perf/261025/page-walk/baseline.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| map 4K 1 | ns/page | ns | 96 | 1.67 | 1.68 | 1.82 | 2.25 | 1.72 | 0.11 |
| map 4K 1 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 1 | ns/page | ns | 96 | 3.10 | 3.51 | 4.26 | 4.38 | 3.66 | 0.46 |
| protect 4K 1 | visits/page | tables | 96 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| unmap 4K 1 | ns/page | ns | 96 | 3.75 | 3.92 | 4.35 | 4.93 | 4.00 | 0.24 |
| unmap 4K 1 | visits/page | tables | 96 | 10.0 | 10.0 | 10.0 | 10.0 | 10.0 | 0 |
| map 4K 512 | ns/page | ns | 96 | 1.16 | 1.18 | 1.19 | 1.61 | 1.19 | 0.0752 |
| map 4K 512 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 512 | ns/page | ns | 96 | 0.99 | 1.00 | 1.03 | 1.72 | 1.02 | 0.086 |
| protect 4K 512 | visits/page | tables | 96 | 4.01 | 4.01 | 4.01 | 4.01 | 4.01 | 0 |
| unmap 4K 512 | ns/page | ns | 96 | 0.95 | 0.95 | 0.97 | 1.07 | 0.96 | 0.0176 |
| unmap 4K 512 | visits/page | tables | 96 | 4.01 | 4.01 | 4.01 | 4.01 | 4.01 | 0 |
| map 4K 32768 | ns/page | ns | 96 | 1.16 | 1.17 | 1.30 | 1.55 | 1.20 | 0.081 |
| map 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| protect 4K 32768 | ns/page | ns | 96 | 0.98 | 1.01 | 1.10 | 1.36 | 1.03 | 0.0705 |
| protect 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| unmap 4K 32768 | ns/page | ns | 96 | 0.93 | 0.94 | 1.02 | 1.37 | 0.97 | 0.0665 |
| unmap 4K 32768 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| map 2M 512 | ns/page | ns | 96 | 0.0025 | 0.0038 | 0.0038 | 0.0076 | 0.00351 | 0.000814 |
| map 2M 512 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 512 | ns/page | ns | 96 | 0.0051 | 0.0076 | 0.0089 | 0.014 | 0.00762 | 0.00167 |
| protect 2M 512 | visits/page | tables | 96 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0 |
| unmap 2M 512 | ns/page | ns | 96 | 0.0063 | 0.0076 | 0.0089 | 0.0102 | 0.00763 | 0.000874 |
| unmap 2M 512 | visits/page | tables | 96 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0.0176 | 0 |
| map 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0032 | 0.0063 | 0.00266 | 0.00058 |
| map 2M 32768 | visits/page | tables | 96 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0.0059 | 0 |
| protect 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0026 | 0.0063 | 0.00228 | 0.000829 |
| protect 2M 32768 | visits/page | tables | 96 | 0.006 | 0.006 | 0.006 | 0.006 | 0.006 | 0 |
| unmap 2M 32768 | ns/page | ns | 96 | 0.0013 | 0.0025 | 0.0026 | 0.0038 | 0.00211 | 0.000615 |
| unmap 2M 32768 | visits/page | tables | 96 | 0.006 | 0.006 | 0.006 | 0.006 | 0.006 | 0 |
| bulk-map 4K 32 | ns/page | ns | 96 | 0.88 | 1.10 | 1.43 | 1.64 | 1.12 | 0.18 |
| bulk-map 4K 32 | visits/page | tables | 96 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 0 |
| yardstick zero-page | ns/page | ns | 96 | 14.7 | 15.0 | 15.2 | 15.5 | 15.0 | 0.17 |
| yardstick store-entry | ns/page | ns | 96 | 0.0699 | 0.0712 | 0.075 | 0.0941 | 0.0724 | 0.0037 |

What it says:

- **The counts are what item 1 says.** A 4 KiB map visits 4 tables a page, against a floor of 4 tables for 512 pages: 512 times it. Protect and unmap visit 6 more per call, `split_ends()`' two walks down to the second level. That is 10 per page for one page, and why 4.01 at 512.
- **The time is small.** 1.17 ns (map), 1.01 ns (protect) and 0.94 ns (unmap) a page at the median for 32768 pages, with p90 within 0.13 ns. The re-walked upper tables stay in the cache, so a walk is a few cached loads.
- **A single-page call costs 1.7 to 3.9 ns.** That includes the call and `split_ends()`.
- **2 MiB leaves cost under 0.01 ns a page,** since one walk serves 512 pages.
- **The bulk map** costs 1.10 ns a page, the same as a contiguous map: the per-call overhead is small next to the walk.
- **The yardsticks:**
  - Zeroing a page costs 15.0 ns. That is what a fault spends on the page it allocates.
  - Storing an entry costs 0.07 ns. That is the part no walk can save, and it is optimistic: the compiler vectorises the loop.

So a walk that went down once per table would save at most about 1 ns per 4 KiB page, at most about 7 % of zeroing the page, and less beside a TLB invalidation. The numbers do not support items 1 and 2 as a priority. The costs around the engine (items 3 to 6) are where the time is likely to be, and they need measuring in the kernel.

## Stages
None

## Conclusion
None: measuring. Whether to optimise items 1 and 2 anyway, as an exercise, or to measure items 3 to 6 first, is the user's to decide.
