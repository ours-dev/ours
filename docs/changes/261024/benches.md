# benches changes

## Title: largepage.bench measures 4 KiB pages against 2 MiB leaves, and skips what an emulator cannot measure (2026-10-24)

**Files**
- `benches/kernel/include/bench/run.hpp` — new: `bench::Needs { AnyMachine, Hardware }` and `bench::Run`, the convention for a benchmark that runs inside the kernel
- `benches/kernel/BUILD.gn` — new: `:bench` (the header) and `:commands`, the group of in-kernel benchmarks
- `benches/kernel/mm/large_page_bench.cpp` — new: the `largepage.bench` console command
- `benches/kernel/mm/BUILD.gn` — `large-page-bench` for the kernel toolchain; `vma-walk-bench` now defined only for the host one
- `kernel/top/BUILD.gn` — links `//benches/kernel:commands` beside `//kernel/commands` when the kernel has a shell
- `.agents/tasks/large-pages/PLAN.md`, `DESIGN.md` — step 6 as done, and D2 kept
- `CLAUDE.md`, `AGENTS.md` — the convention, beside the rule that benchmarks go in `benches/`

**Symptom**
None, a new capability. Large-pages step 6: the design said the measurement decides D2's default (a flag on the VMO, or large pages for every eligible fault). The only x86 machine here is QEMU's translator (TCG) on an arm64 host, which has no TLB of its own worth the name, so the measurement had to say which of its numbers mean something there.

**Cause**
What a benchmark reports splits in two, and the split is about the machine, not the code:

- A count or a size (faults taken, tables built, pages committed) is a property of the kernel's algorithm. Any machine that runs the kernel's instructions gives the same number.
- A timing that rests on the processor's caches or TLB is a property of the processor. Under TCG, QEMU keeps its own software TLB, one entry per 4 KiB guest page whatever the guest's page size, so a guest 2 MiB leaf saves no TLB entries at all. A timing taken there is not a smaller version of the hardware effect; it is a different quantity.

Printing both kinds side by side without saying which is which invites reading the second as if it were the first. The user's call: benchmarks tell the emulator from real hardware.

**Change**
The convention, in `bench/run.hpp`: each metric says what it needs, and one `bench::Run` per command decides.

```cpp
bench::Run const run("largepage.bench", argc, argv);  // prints "machine: emulated"
auto const reads = run.measures("random reads", bench::Needs::Hardware);
// on an emulator: "skip  random reads: needs the processor's caches or TLB ..."
```

`all` as the argument runs a hardware metric anyway and labels its number "not a hardware number": the only way to exercise that code on a machine that cannot give its number, so it is not committed untested.

The benchmark maps a 64 MiB object of each kind (plain, and `VmoFlags::LargePages`) into an address space of its own, placed by the VMM, and measures:

| Metric | Needs | How |
|---|---|---|
| faults | any machine | a byte written to every page through `copy_to_user_capture_faults()`; each captured fault counted, then resolved by `soft_fault()` and the copy redone |
| first touch | any machine | the time that pass takes |
| 2 MiB leaves, page tables | any machine | each 2 MiB window queried in the page table: a leaf or a table of 4 KiB entries; plus `PageTableDeposits::nr_held()` before and after |
| committed | any machine | a byte written to one page in every 2 MiB of a fresh object; `committed_pages()` |
| random reads | hardware | 65536 8-byte `copy_from_user()` reads of pseudo-random pages, all present, the same sequence for both kinds |

Faults are captured rather than taken through the resolving copy so that each one is counted exactly, without adding a counter to the fault path: a captured fault is one real #PF, trapped and handed back, then resolved by the same handler a user fault reaches.

The build: `benches/kernel/mm/BUILD.gn` is now read in two toolchains, the host one for `vma-walk-bench` and the kernel one for `large-page-bench`, so each target is defined only in its own (`if (!is_kernel)`); in the kernel toolchain `//kernel/mm:headers.config` is already a default config and adding it again is a GN error. The group `//benches/kernel:commands` lives under `benches/kernel/` and not in `//benches/BUILD.gn`, because that file is read in the default toolchain, where every target is resolved and the kernel's headers cannot be.

Rejected:
- A `vm.page_faults` counter in the fault path, read before and after. It would count every fault on the system, and it is a change to mm for a benchmark's sake; capture counts exactly the bench's own.
- Free frames before and after for the page-table memory. `free_frames()` counts the buddy allocator only, not the per-CPU caches, so it moves in batches; the page table itself says exactly which windows have a table.

**Reference**
Linux measures the same things with userspace tools, not in the kernel: `tools/testing/selftests/mm/` (e.g. `khugepaged.c`, `split_huge_page_test.c`) for behaviour, and `perf stat -e page-faults,dTLB-load-misses` for the numbers, where the PMU events are what this benchmark cannot have under TCG. Zircon's in-kernel benchmarks, `benchmarks()` in `kernel/tests/benchmarks.cc`, registered as the `bench` console command in `kernel/tests/tests.cc`, run on the console as this one does, with no notion of an emulator; they also disable preemption and the watchdog first so that nothing else lands in a timing. The Needs split is this project's own design: it exists because its only x86 machine is an emulator.

**Verification**
- Build, clang-format, clang-tidy: clean.
- `python3 scripts/kshell_run.py --out out/bringup.x86 largepage.bench` (QEMU TCG, 4 CPUs, 512 MiB), three runs:

| Metric | 4 KiB | 2 MiB |
|---|---|---|
| faults | 16384 | 32 |
| first touch | 184 to 294 ms | 37 to 40 ms |
| 2 MiB leaves | 0 | 32 |
| page tables (in the tree + set aside) | 32 | 32 |
| committed after one page per 2 MiB | 128 KiB | 65536 KiB |
| per random read (`all` only, not a hardware number) | 246 ns | 244 ns |

- Reading them:
  - faults: 512 times fewer, exactly the block's page count.
  - first touch: 5 to 7 times faster, not 512, because both kinds zero the same 64 MiB and make the same 16384 copies; only the traps go away. The difference, about 150 to 250 ms over 16352 faults, puts one emulated fault at roughly 10 to 15 µs.
  - page tables: equal. A leaf holds the very table its 4 KiB mapping would have used, set aside instead of in the tree, so deposits cost no memory a 4 KiB mapping would not.
  - committed: 512 times more for a sparse access. One page touched commits its whole block. This is the cost, and it is measured exactly here.
  - random reads: no difference under TCG, as expected; it is skipped without `all`.
- `boot_check.py`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), host tests 146 passed.
- The TLB effect, the reason large pages exist: not measured, no bare-metal or KVM x86 machine at hand.

**Deliberately not done**
- D2 stays the flag (the user's call, 2026-10-24): what this machine measures exactly is the cost (512 times the memory for a sparse access); the benefit it can show (fewer faults) is real but smaller in time than in count, and the main benefit (TLB reach) it cannot show at all. Large pages for every eligible fault waits for hardware or KVM numbers.
- No TLB-miss counts: under TCG there is no PMU, and on hardware the DTLB events are model-specific; that needs PMU support of its own.
- The DESIGN's 256 MiB object and sequential reads: 64 MiB already holds 32 blocks, enough for every count to be exact, and fits twice over in the dbg image's 512 MiB; sequential reads, like random ones, are a TLB question and wait with them.
- Preemption stays on while timing, unlike Zircon's `benchmarks()`: a fault may block and needs it, and on this emulator the timings are not the point. The random reads, which never fault, are where turning it off would matter, once there is hardware to run them on.
- Not in `boot_check.py`: a benchmark reports, it does not pass or fail, and it takes about two seconds of an emulated boot.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why is first touch only 5 to 7 times faster with 512 times fewer faults?
- Why do the 2 MiB mappings use exactly as many page tables as the 4 KiB ones, and where are those tables?
- Why does a timing of random reads under QEMU TCG say nothing about large pages, while the fault count does?
