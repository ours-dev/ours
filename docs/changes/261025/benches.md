# benches changes

## Title: page-walk-bench counts the tables the paging engine enters, and times its map, protect and unmap on the host (2026-10-25)

**Files**
- `benches/kernel/lib/arch/page_walk_bench.cpp` — new: the host benchmark
- `benches/kernel/lib/arch/BUILD.gn` — new: `page-walk-bench`, host toolchain
- `benches/BUILD.gn` — `//benches:benches` builds it
- `docs/repairs/261025/arch-paging.md` — items 1 and 2 carry the numbers

**Symptom**
None, a new benchmark. Items 1 and 2 of `docs/repairs/261025/arch-paging.md` ask for a measurement before anything changes, and the user asked for the data first (2026-10-25).

**Cause**
None, a new benchmark.

**Change**
The engine is header-only, so the bench calls `arch::Paging<X86Paging>` directly, on page tables in an arena whose addresses stand in for physical ones. Objects:

- **A counting `PhysToVirt`.** The engine calls it once each time it enters a table, so the count is the number of tables read. It is exact and the same on any machine.
- **The floor.** The number of distinct tables the range touches: what a walk that went down once per table would read.
- **The time per page.** The engine's walks are software on ordinary memory and touch no TLB, so host time is a fair number for them. It leaves out the lock, interrupts off and the shootdown that the kernel adds around them.
- **Two yardsticks.** Zeroing one 4 KiB page, and storing entries with no walk at all.

Cases:

- map, protect and unmap of 1, 512 and 32768 pages with 4 KiB leaves (frames a page off a 2 MiB boundary);
- the same with 2 MiB leaves (deposits always granted);
- a 32-page bulk map done as `PageTable::map_pages_bulk()` does it, one engine call a page.

**Reference**
None, this project's own benchmark. It sits beside `vma-walk-bench` and mirrors the source path of the code it measures.

**Verification**
Three runs on the host (Apple M5 Pro) gave the same counts and times within a few percent:

- **4 KiB leaves:** a map visits 4 tables a page, so 2048 for 512 pages against a floor of 4, and 131072 for 32768 against 67. Protect and unmap visit 6 more, from `split_ends()`. All three cost 0.95 to 1.26 ns a page.
- **2 MiB leaves:** 3 visits a leaf.
- **Bulk map:** 4 a page, at 1.1 ns a page.
- **Yardsticks:** zeroing a page 28.7 to 34.5 ns; storing an entry 0.07 ns.

The full table is in item 1 of `docs/repairs/261025/arch-paging.md`.

**Deliberately not done**
- Timing inside the kernel: the walk is the same code, and what the kernel adds (locks, interrupts off, invalidation) is items 3 to 6, which need their own measurement.
- Running it from the host-test list: it reports numbers and passes or fails nothing, like `vma-walk-bench`.

**How it was found**
None

**The user's prediction**
The user expected a large room for optimisation in the engine (2026-10-25). The counts bear that out (up to 1956 times the floor). The time does not: about 1 ns a page, since the re-walked tables stay in the L1 cache.

**Three questions you should be able to answer**
- Why does a walk that reads 1956 times as many table entries as it must cost only about 1 ns a page?
- What do protect and unmap visit that map does not, and why exactly 6 tables more?
- Which of the engine's costs would this bench not show, and where would they have to be measured?
