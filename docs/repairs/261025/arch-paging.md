# arch-paging outstanding repairs

Performance of the paging engine (`kernel/lib/arch/include/arch/paging/mod.hpp`) and of the x86 layer over it (`kernel/arch/x86/mm/page_table.cpp`, `arch_vm_aspace.cpp`), recorded at the user's request on 2026-10-25. None of it is measured yet. Each item says what to measure first, and is decided with the user item by item before anything changes (CLAUDE.md).

## 1. Every leaf of a range is found by a walk from the root

**Severity**
Low: performance only. It is paid on every map, unmap and protect of more than one leaf.

**Symptom**
Not measured. A map, unmap or protect of N final-level pages walks the four levels N times. A 512-page populate reads 2048 entries to write 512.

**Cause**
- **map()** calls `map_one_from<PAGING_LEVEL>()` once per leaf, and each call starts at the root (`while (context.has_more())`).
- **unmap() and protect()** do the same through `walk_levels<PAGING_LEVEL>()` in a `while (remaining != 0)` loop, one leaf per pass (`unmap()`, `visit_range()`).
- **split_ends()** adds two more root walks to every unmap and protect, even where no large leaf exists.

The upper levels are re-read although they change only at a table boundary: once per 512 pages at the final level.

**Change**
Measure first, a bench in `benches/kernel/`:

- entries read per page, and time per page;
- for map, unmap and protect;
- of 1, 512 and 32768 pages, at 4 KiB and with 2 MiB leaves.

Entries read is exact even under QEMU's translator.

Candidates:

- **Walk once per table, then step through it.** Descend to a table once, and visit its entries in a loop up to the table's end or the range's, before going back up. This is what Linux's `*_pte_range()` loops and Zircon's `AddMapping()` / `RemoveMapping()` recursion do. Cost: the visitor interface becomes range-shaped instead of one-leaf-shaped, which touches query, unmap, protect and map.
- **Cache the last table per level in the walk context**, and start the next step from the deepest cached table whose range still covers the address. Smaller change, same asymptotic gain for runs, and no help for scattered pages.

**Reference**
Linux `zap_pte_range()`, `remap_pte_range()`, `change_pte_range()` (`mm/memory.c`, `mm/mprotect.c`). Zircon `X86PageTableBase::AddMapping()` and `RemoveMapping()` (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`).

**Acceptance**
The bench's entries-read-per-page for a 512-page map, unmap and protect, before and after, recorded with the user's decision.

## 2. A bulk map is one engine call per page

**Severity**
Low: performance only. It is paid on every populate and every coalescer batch of up to 32 pages (`VmMapping::map_paged()`).

**Symptom**
Not measured. `PageTable::map_pages_bulk()` calls `Engine::map()` with one page per call. Each call repeats everything the engine does per call:

- validates the options and the range;
- builds a `MapContext`;
- walks from the root.

**Cause**
`Paging::map()` takes one physical address and maps a contiguous run from it. Frames that are not contiguous therefore need a call each. Meanwhile `PhysAddrCursor` already holds an array of addresses (`PhysAddrCursor(PhysAddr *phys, usize n, usize page_size)`), and `MapContext` is built with a count of 1 only because `map()` has no way to pass more.

**Change**
Measure with item 1's bench: map_bulk of 32 scattered frames, time and entries read.

Candidate: a `map()` that takes a `PhysAddr` array and a page size. One walk then serves the whole batch. With item 1, it is one descent per final-level table.

Cost:

- **Failure handling moves into the engine.** The per-call failure choice is carried out by `map_pages_bulk()` today.
- **The leaf-size choice changes.** It has to use the array's page size, not the run's alignment.

**Reference**
Zircon `ArchVmAspaceInterface::Map()` takes an array (`paddr_t* phys, size_t count`) into one `MappingCursor` and one `AddMapping()` walk.

**Acceptance**
The bench's time per page of a 32-frame bulk map, before and after.

## 3. Invalidation is one invlpg per 4 KiB page, with no ceiling and no regard for leaf size

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

## 4. Each page-table operation is its own shootdown

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

## 5. No PCIDs: every switch between processes flushes the TLB

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

## 6. Interrupts are off for the whole of a range operation

**Severity**
Low: latency only. It grows with the size of a map, unmap or protect, on the CPU doing it.

**Symptom**
Not measured. `PageTable::map_pages()`, `map_pages_bulk()`, `unmap_pages()` and `protect_pages()` hold `m_lock`, a spin lock, with `IrqSave` for the whole range. While interrupts are off, the CPU may:

- allocate and zero page tables (`alloc_table()`);
- withdraw deposits and fill split tables;
- walk thousands of entries (item 1).

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
