# arch-paging outstanding repairs

Performance of the paging engine (`kernel/lib/arch/include/arch/paging/mod.hpp`) and of the x86 layer over it (`kernel/arch/x86/mm/page_table.cpp`, `arch_vm_aspace.cpp`), recorded at the user's request on 2026-10-25. Item 1 is measured; the others are not yet. Each item says what to measure first, and is decided with the user item by item before anything changes (CLAUDE.md).

## 1. A one-page map runs 26 more instructions than the engine's

**Severity**
Low: performance only. It is paid on every one-page map, and a page fault maps one page (`VmMapping::page_fault()`), but it is about 1.1 ns next to a fault that zeroes a page (15 ns in the same bench) and enters the kernel.

**Symptom**
After stage 6 of `docs/perf/261025/page-walk.md`, a one-page map costs 4.9 ns and runs 151 instructions (counted with lldb in the bench's steady state), against the engine's 3.8 ns and 125 (`page-walk/stage-2-beside-6.csv`, `stage-6-beside-2.csv`). One-page protect and unmap are at or under the engine's (117 and 123 instructions against 141 and 152). History: stage 2's range walk took a map from 90 to 125 instructions and 2.55 to 3.96 ns; stage 3's CRTP base with the level an argument to 357 and 11 ns; stages 4 to 6 brought it to 151 and 4.9.

**Cause**
Not isolated. What the instruction-by-instruction traces of a one-page map (lldb, the 21st call, the engine's against stage 6's, 125 against 151 instructions) show:

| | engine | stage 6 |
|---|---|---|
| stack: registers saved and restored, frame | 9 | 23 |
| call / return | 1 | 3 |
| branch | 26 | 33 |
| load | 17 | 19 |
| store | 5 | 9 |
| arithmetic / moves | 67 | 64 |

(lldb places a breakpoint after the function's own prologue, so the entry function's prologue is in neither count, and `map_with()`'s, which is called, is in stage 6's.) The map is two functions in stage 6, `map()` and `map_with()`, with a `MapCursor` built on the first one's stack and passed to the second by reference; in the engine it is one function.

Two explanations were tested and do not hold:

- **The second function.** With `map_with()` forced inline, a one-page map runs 156 instructions (151 before) and 5.1 ns (5.4), a 32-frame bulk map 1138 (1139): within the noise.
- **The frames being read through a plain `unsigned long *`**, which the compiler might take for an alias of the cursor's own fields (the kind of aliasing stage 4 fixed). With the frame addresses wrapped in a struct, the counts, the stores and the times were identical to the instruction.

The bulk map shows the same difference in its page loop: 24 instructions a page against the engine's 14, of which 4 are stores of the cursor (`stp`, `str`, `str`, `str xzr`) and 4 the generic cursor's check for the end of a frame's run, which the engine's `map_frames` has folded away (a run is one page there). The stores stay with `map_with()` inlined. A copy of the cursor in a local variable inside the last-level loop took the bulk map from 1.11 to 0.98 ns a page (exploratory), so the stores cost at most 0.13 of the 0.45 ns it is behind. Why the compiler keeps the cursor in memory in these loops is not known.

**Change**
Measure first, as a stage of `docs/perf/261025/page-walk.md`: the one-page map's instructions (lldb) and time, with ranges and the bulk map checked for no loss.

Next diagnostic step, before any candidate: compile the walk to LLVM IR (`-S -emit-llvm`) and see where the cursor's address leaves the function, and what `-Rpass-missed` says about the loop.

Candidates, to put to the user:

- **A short path for a range inside one final-level table.** When every table on the way exists, go straight down as the old walk did and run only the final level's loop. It covers a fault, and a fault-around of up to 512 pages. Cost: a second way down, which must agree with `map_range()` on every option.
- **A cursor for a contiguous run.** Template `map_range()` on its cursor: `map()` gets a physical address and a count, `map_frames()` the array. Cost: two instantiations of the walk, on top of the level's.
- **Leave it.** 1.1 ns is small next to the rest of a fault.

**Reference**
Zircon's `X86PageTableBase::AddMapping()` and `AddMappingL0()` have the same loops at every level and no short path for one page (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`). Linux's fault path does not walk a range: `do_anonymous_page()` writes the entry the fault found (`mm/memory.c`).

**Acceptance**
A one-page map runs no more instructions than the engine's 125, or the user's decision to leave it is recorded in the report; ranges and the bulk map keep their times.

## 2. Invalidation is one invlpg per 4 KiB page, with no ceiling and no regard for leaf size

**Severity**
Low: performance only. It grows with the size of every unmap and protect, on every CPU in the shootdown.

**Symptom**
Not measured. `ArchVmAspace::invalidate(va, n)` runs `invlpg` once per page on every target CPU, inside one IPI. Two consequences:

- **Unmapping 64 MiB** is 16384 `invlpg` on each CPU running the address space, with the initiator waiting for all of them.
- **A 2 MiB leaf costs 512 `invlpg`.** A single `invlpg` at any address inside a large leaf drops that leaf's TLB entry; the other 511 only repeat it.

**Cause**
`invalidate()` knows a page count and nothing about the leaves that held the range, and it has no point at which a whole flush becomes cheaper.

**Change**
Measure: `invlpg` count and shootdown time per unmap of 1, 512, 32768 pages, at 4 KiB and with 2 MiB leaves.

- The count is exact anywhere.
- The time needs hardware: TCG's TLB is not the CPU's, so mark it hardware-only in `bench::Run`.

Candidates:

- **A ceiling.** Past N pages, reload CR3, or use INVPCID's all-context form for Global entries, instead of N `invlpg`. Linux uses 33 (`tlb_single_page_flush_ceiling`, `arch/x86/mm/tlb.c`); the right N here is what the measurement says.
- **Per-leaf invalidation.** The engine reports the leaves it changed with their sizes, and `invalidate()` issues one `invlpg` per leaf. Zircon's `PendingTlbInvalidation` records each item's page level for this.

**Reference**
Linux `flush_tlb_mm_range()` and `tlb_single_page_flush_ceiling`. Zircon `PendingTlbInvalidation` (`page_tables.h`), whose `item[32]` turns into `full_shootdown` past its size.

**Acceptance**
The bench's `invlpg` count for a 2 MiB-leaf unmap drops from 512 to 1, and shootdown time on hardware is recorded for the ceiling the user picks.

## 3. Each page-table operation is its own shootdown

**Severity**
Low: performance only. It is paid by any one logical change that touches several ranges.

**Symptom**
Not measured. One change that reaches several mappings sends one synchronous IPI round per range and waits for each. Two examples:

- `VmCowPages::change_range_locked()` takes Write off a node's pages in every mapping of every object over it (node, then attachments, then objects, then mappings).
- A `take_pages()` unmaps in the same way.

**Cause**
`ArchVmAspace::unmap()` and `protect()` invalidate before they return. Nothing can gather invalidations from several calls and flush once.

**Change**
Measure: IPI rounds per `take_pages()` and per clone's remove-write, over objects with 1, 4 and 16 mappings.

Candidate: a gather object that the operations append to, flushed once at the end of the logical change (Linux `mmu_gather`, Zircon `ConsistencyManager`).

Cost:

- **Every caller must flush before anything relies on the change.** In particular, the caller must not free a frame before the flush. That is the use-after-free that per-call shootdown rules out today.
- **The deferred-free list belongs to the gather.**

**Reference**
Linux `struct mmu_gather` (`include/asm-generic/tlb.h`), `tlb_finish_mmu()`. Zircon `X86PageTableImpl::ConsistencyManager` (`page_tables.h`).

**Acceptance**
IPI rounds per clone's remove-write over 16 mappings, before and after, with the user's decision on where the gather lives.

## 4. No PCIDs: every switch between processes flushes the TLB

**Severity**
Low: performance only. It is paid on every context switch between two user address spaces.

**Symptom**
Not measured. `ArchVmAspace` never allocates a PCID (`m_pcid` stays `INVALID_PCID`). Every CR3 load therefore drops every non-Global translation, and a process switched back in refills its TLB from its page tables.

**Cause**
PCID support was left out. The shootdown's correctness argument relies on it: "no PCIDs: loading another root flushed it" (`ArchVmAspace::invalidate()`).

**Change**
Measure on hardware: time of a ping-pong between two processes over a channel, and TLB misses after each switch if the PMU sampler can count them.

Candidate: allocate PCIDs, as Zircon's `PCIDAllocator` does (`arch/x86/mmu.cc`), and load CR3 with the no-flush bit.

Cost: the shootdown can no longer assume a CPU that switched away holds nothing.

- A CPU that ran the address space earlier may still hold its translations under that PCID.
- So invalidation must reach it too, or bump the address space's PCID generation, or flush that PCID on its next switch in.

That is a correctness change and goes through the invariant argument, not a measurement.

**Reference**
Zircon `PCIDAllocator` and `X86ArchVmAspace::ContextSwitch()` (`arch/x86/mmu.cc`, counter `mmu.context_switches_pcid`). Linux `switch_mm_irqs_off()` and its ASID generations (`arch/x86/mm/tlb.c`).

**Acceptance**
The ping-pong time on hardware before and after, recorded with the user's decision on how a CPU that ran the address space earlier is kept correct.

## 5. Interrupts are off for the whole of a range operation

**Severity**
Low: latency only. It grows with the size of a map, unmap or protect, on the CPU doing it.

**Symptom**
Not measured. `PageTable::map_pages()`, `map_pages_bulk()`, `unmap_pages()` and `protect_pages()` hold `m_lock`, a spin lock, with `IrqSave` for the whole range. While interrupts are off, the CPU may:

- allocate and zero page tables (`alloc_table()`);
- withdraw deposits and fill split tables;
- walk thousands of entries.

**Cause**
One spin lock with interrupts off guards the page table for every caller. Some of those callers edit the kernel's tables with interrupts already off: the vmalloc self-test and `reserve()` at boot (`ArchVmAspace::invalidate()`'s comment).

**Change**
Measure: the longest interrupts-off stretch inside these functions, as a histogram, for a 32 MiB populate and unmap.

Candidates:

- **Chunk long operations.** Let go of the lock every N tables. Each chunk is then consistent on its own, and with UndoOnFailure the undo still covers what was done.
- **A sleeping lock for user address spaces.** Zircon guards its page tables with a `Mutex`. That needs every user page-table edit to happen in thread context.

**Reference**
Zircon `X86PageTableBase::lock_` (a `Mutex`). Linux takes the PTE lock per final-level table (`pte_offset_map_lock()`), never across a whole range.

**Acceptance**
The histogram for a 32 MiB populate and unmap, before and after, with the user's decision.
