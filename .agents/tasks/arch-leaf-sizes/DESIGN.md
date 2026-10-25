# arch-leaf-sizes: the page table chooses a mapping's leaf sizes, and mm only offers contiguous runs

**Date**: 2026-10-25
**Status**: Proposed

## Problem
The user asked on 2026-10-25 whether `ArchVmAspace::large_leaf_slot_empty()` and its comment are reasonable, and what happens when 1 GiB or larger leaves are wanted. As built, they are not ready for a second leaf size, in three ways.

**The interface is named for one size.** Its comment says 2 MiB, and the x86 implementation forwards to the engine's `second_level_empty()`, so a page-table level number is part of what mm calls.

**Generic mm hard-codes the hardware's leaf size.**

- `VmCowPages::BLOCK_SIZE = 2_mb` is documented as "the 512 pages that one 2 MiB leaf of a page table maps".
- Everything that deals in large pages follows that constant:
  - `LARGE_LEAF` in `vm_mapping.cpp`;
  - the placement phase in `vm_area.cpp` (large-pages D6);
  - `VmObjectPaged::find_block_locked()`;
  - `VmCowPages::fill_block_locked()`, which allocates order 9.
- That size is the geometry of x86, and of arm64 with 4 KiB granules. Other machines differ:
  - arm64 with 16 KiB granules has 32 MiB blocks, and with 64 KiB granules 512 MiB blocks.
  - arm64's contiguous bit makes a 64 KiB or 32 MiB leaf out of 16 entries.
  - RISC-V Sv39 has 2 MiB and 1 GiB leaves.

**mm decides that a window is a leaf, then asks the arch whether the slot is free, then maps.** `VmMapping::map_block_locked()` calls `large_leaf_slot_empty()` and then `ArchVmAspace::map()`. This is check-then-act. It is safe only because the address-space lock is held across both calls, and nothing says so.

Meanwhile, the paging engine already chooses leaf sizes by itself. `arch::paging::Paging::map()` (`kernel/lib/arch/include/arch/paging/mod.hpp`) maps "with a leaf as large as the range and its alignment allow at each step". `map_block_locked()` relies on exactly that when it maps a block with one contiguous `map()` of 512 pages. So the size knowledge exists twice: the engine knows which leaves exist and fit, and mm repeats one of those answers as a constant.

Nothing is broken today. The large-pages task deferred 1 GiB user leaves (its D3), and only x86 has paging traits. This design is about where the seam goes, so that the next leaf size is an arch change and not an mm rewrite.

## Goals and non-goals
Goals:

- No hardware leaf size in generic mm. mm states what it knows about a range, and the arch layer chooses the leaves. What mm knows:
  - the pages are physically contiguous, and how far;
  - how aligned the range is;
  - there is one protection over it.
- A new leaf size changes the arch layer and the block policy only. Examples: x86 1 GiB, arm64 contiguous ranges, RISC-V gigapages.
- Large-pages D1 still holds for every leaf size the arch maps in a user address space: a split never allocates.
- Checking the slot and writing the leaf are one step, under the page table's own lock.

Non-goals:

- Building 1 GiB user leaves, or paging for any architecture but x86, now.
- Changing what a VMO allocates: it is still 2 MiB blocks under `VmoFlags::LargePages`.
- Leaves in the kernel address space (large-pages D5).

## Design
Objects:

- **`LeafGeometry`**, one per arch: the leaf sizes a user page table maps and can split without allocating, largest first. With each size, what has to be set aside when a leaf of that size is mapped, so that its split never allocates:
  - x86 today: 2 MiB, with one PT table set aside.
  - x86 with 1 GiB: one PD table, plus one PT table for each of the 512 2 MiB leaves the split makes. That is large-pages D3's arithmetic, about 2 MiB of tables per 1 GiB leaf.
- **`ArchVmAspace::map_run(va, pa, len, flags) -> usize`**: maps a physically contiguous run, and returns how many pages it mapped. At each step it uses the largest leaf of `LeafGeometry` that meets all of these, and smaller entries otherwise:
  - its size divides both the virtual and the physical address;
  - it fits in what is left of the run;
  - its slot is empty;
  - what has to be set aside for it is available.

  It replaces both `large_leaf_slot_empty()` and the 512-page `map()`. The slot check moves into the engine, under the page table's lock, so the check and the write cannot be separated.
- **A block**: pages a VMO allocated together, physically contiguous and aligned to their size. Its size is the VMO's allocation policy, not a page-table fact. `VmoFlags::LargePages` takes the smallest `LeafGeometry` size above 4 KiB unless told otherwise.
- **`VmObjectPaged::contiguous_run_locked(offset, limit)`** replaces `find_block_locked()`. It answers with the longest run around `offset` of pages that meet all of these, clipped to `limit`:
  - held by one node;
  - physically contiguous;
  - laid out the same way in the object and in physical memory.

  `limit` is the mapping, the protection region, and `LeafGeometry::largest()`, intersected. Like `find_block_locked()` today, it also says whether the run may be mapped with Write.
- **Placement** (large-pages D6) aligns a large-page mapping to the VMO's block size, which it reads from the VMO.

Events, a fault on a large-page VMO:

1. The fault finds its page as it does today.
2. mm asks the object for the contiguous run around the page, clipped to the mapping, the protection region and the largest leaf.
3. mm calls `map_run()` with that run.
4. The engine walks the run and picks the leaves; at worst it uses 4 KiB entries.
5. mm learns how many pages were mapped, as it does today, and nothing about their sizes.

Events, a populate: the same, once for each run in each protection region.

Locking:

- The address-space lock and the VMO lock are held as they are today.
- The engine takes the page table's lock for the check and the write together.
- Nothing new nests.

The divergent part: mm offers runs, not leaves. The arch is then free to use features mm never hears of. arm64's contiguous bit is one: 16 adjacent entries cached as one TLB entry. It needs exactly "a 64 KiB run, contiguous and aligned in both addresses", and that is what this interface already describes.

Reference:

- Zircon's page tables choose large leaves for a contiguous run themselves: `X86PageTableBase::AddMapping()` (`arch/x86/page_tables/include/arch/x86/page_tables/page_tables.h`) and `ArmArchVmAspace::MapPageTable()` (`arch/arm64/mmu.cc`). It does this only for physically contiguous VMOs.
- Linux's multi-size THP (6.8) lets each arch support several folio orders (`thp_vma_allowable_orders()`, a per-size `hugepages-<size>kB/enabled`).
- arm64's contpte (6.9, `arch/arm64/mm/contpte.c`) maps such a folio with the contiguous bit, without the core mm knowing.

## Alternatives considered
- **A sized query.** `leaf_slot_empty(va, size)` plus an arch list of sizes, with mm trying sizes from the largest down.
  - Small.
  - But mm still decides leaves, still repeats the engine's knowledge, and still checks and acts in two calls.
- **A rename only.** Document `large_leaf_slot_empty()` as the 2 MiB slot and leave the rest. Honest, but it extends to nothing.
- **Per-level paths in mm** (Linux's original THP). Separate fault, split and copy code for each leaf level: `do_huge_pmd_anonymous_page()` for one level, `create_huge_pud()` and `wp_huge_pud()` for the next. Each new level is a new copy of the core paths, which is the duplication this design avoids.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| R1. Who chooses a mapping's leaf sizes | mm, with a sized query (alternative 1); the arch, from runs mm offers | the arch | None (homework paused) | |
| R2. What sets a VMO's block size | a constant, as today; the VMO's allocation policy, chosen from `LeafGeometry` | the VMO's policy | None | |
| R3. When | all now; `map_run()` now (it removes the level name from the interface and the check-then-act) and the block policy with the first second leaf size | `map_run()` now, the policy later | None | |

## Testing and verification
- `largepage.test` (27 checks) passes unchanged: the same VMOs get the same leaves.
- `x86-paging-smoke` (host), `map_run()` in these cases:
  - a run 2 MiB aligned in the virtual address but not in the physical one maps 4 KiB entries;
  - a run over a slot that already holds a table maps 4 KiB entries there and 2 MiB leaves elsewhere;
  - a run with nothing set aside available maps 4 KiB entries;
  - a 1 GiB aligned run, with 1 GiB absent from `LeafGeometry`, maps 2 MiB leaves.
- Control run: with the slot check removed from the engine, the occupied-slot case fails.
- `ours-verify` on every step.

Cannot be verified: any architecture but x86, since nothing else has paging traits yet.

## Open questions
- How a 1 GiB leaf's tables are set aside: all 513 tables up front, which keeps D1; or the PD table only, with the PT tables allocated when a 2 MiB leaf from the split is itself split, which allocates and breaks D1.
- Whether `LeafGeometry` is fixed at compile time or read at boot. x86 1 GiB pages need CPUID's `PDPE1GB`; arm64 granule sizes are a boot choice.
- Whether `map_run()` should report the leaves it used, for statistics and tests, or only a page count, as `map()` does.
