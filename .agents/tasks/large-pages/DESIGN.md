# large-pages: user memory mapped with 2 MiB leaves, split without allocating

**Date**: 2026-10-24
**Status**: Proposed

## Problem
Every user mapping is made of 4 KiB entries. `VmMapping::map_paged()`, `map_physical()` and the fault path all go through `ArchVmAspace::map_bulk()` or a one-page `map()`, which map one page per call; only a contiguous `ArchVmAspace::map(va, pa, n)` lets the paging engine choose a 2 MiB or 1 GiB leaf (`Paging::map_one_from()`), and no user path calls it. A large VMO therefore costs one TLB entry per 4 KiB, and a random walk over it misses the TLB on almost every access.

The engine cannot take a large leaf apart either. `unmap()` and `protect()` of part of a large leaf answer InvArgs (`UnmapVisitor::on_terminal()`, `ProtectVisitor::on_terminal()`), and nothing splits one. That has to be settled before user large pages exist, because operations that touch part of a mapping must not fail: a page leaving a VMO (`take_pages()`, `.agents/tasks/user-pager/` step 4) unmaps under the VMO lock with no way back, and a clone takes Write off part of a mapping the same way. The user paused user-pager at step 4.2 for this (2026-10-24).

The user's decision D1: a split never allocates. Every 2 MiB leaf mapped into a user address space comes with a page table set aside for it when it is mapped (a deposit), and a split fills that table with the leaf's 512 entries.

## Goals and non-goals
Goals:

- The paging engine maps a 2 MiB leaf with a deposited table in a user address space, splits it with that table when an unmap or protect covers part of it, and frees the deposit when the whole leaf goes and when the address space is torn down. None of these allocates or fails.
- A VMO can be backed by 2 MiB blocks (order-9 buddy allocations) and its mappings use 2 MiB leaves over them where the mapping and its protection allow (D2, D6).
- Every operation that changes part of a 2 MiB range -- unmap, protect, remove-write for a clone, a copy-on-write fault, `take_pages()` -- works page by page after a split, as it does on 4 KiB entries today.
- A measurement of what it buys: random and sequential access over a large VMO, 4 KiB against 2 MiB, in `benches/`.

Non-goals: 1 GiB user leaves (D3); large leaves in the kernel address space beyond what boot already maps (D5); putting 4 KiB pages back together into a 2 MiB leaf after a split (D8); reclaim, compaction, or anything that moves pages to make a 2 MiB block.

## Design
Objects:

- A 2 MiB leaf: an entry at the page-directory level (x86 PD) with the large-page bit, naming 512 physically contiguous, 2 MiB aligned frames.
- A deposit: a 4 KiB page-table page, zeroed, allocated when the leaf is mapped and kept unused until the leaf is split or unmapped. The PD table page that holds the leaf entry keeps its deposits in its frame descriptor: the `Mmu` role gets a body with the head of a FIFO of deposited tables, chained through each deposited table's own list hook, which a page-table page uses for nothing else (D4).
- A 2 MiB block of a VMO: an order-9 buddy allocation whose 512 frames are 512 ordinary slots of the node's page map, each with its own descriptor, share count and role. The head's descriptor records that the run is one block, so that a mapping can see with one lookup that the 2 MiB range is mappable as one leaf. A block stops being one the moment any of its pages leaves, is copied on write, or is freed alone; its frames are then 512 order-0 frames to the allocator.

Events, a fault on a large-page VMO (D2):

1. A read or write fault lands in a 2 MiB aligned range of a root node with no page source and no page there yet, and the mapping covers the whole 2 MiB of addresses at a 2 MiB congruent offset (D6) with one protection across it.
2. The node allocates an order-9 block (zeroed) and fills the 512 slots; if the buddy has no 2 MiB block, it falls back to the one 4 KiB page, as today.
3. The mapping maps the 2 MiB leaf. The engine takes a deposit from the allocator first; with none, it maps the 4 KiB page instead, so a deposit is never missing for a leaf that exists.

Events, a split:

1. Something changes part of a 2 MiB leaf of a user address space: an unmap, a protect, a clone's remove-write, a copy-on-write fault on one of its pages.
2. The engine withdraws the leaf's deposit, writes the 512 entries (same frames, same permissions) into it, and replaces the leaf with a pointer to it. The old 2 MiB translation is invalidated on every CPU running the address space before anything relies on the new entries.
3. The operation proceeds on the 4 KiB entries it touches.

Events, a write to a 2 MiB block shared by a clone (D7): the writer's leaf is split, and the one 4 KiB page is forked or migrated as any shared page is (`VmCowPages::cow_page()`); the block in the hidden node stays where it is, and its other 511 pages keep their counts.

Locking: deposits are taken and returned under the page table's own lock, where the entry that owns them changes; nothing new nests under it.

Reference: Linux `__do_huge_pmd_anonymous_page()` (`mm/huge_memory.c`, allocates the folio and `pte_alloc_one()` before mapping, falls back on failure), `pgtable_trans_huge_deposit()` and `pgtable_trans_huge_withdraw()` (`mm/pgtable-generic.c`, a FIFO hung off `ptdesc->pmd_huge_pte`), `__split_huge_pmd_locked()` and `zap_deposited_table()` (`mm/huge_memory.c`), `do_huge_pmd_wp_page()` (splits and falls back to a 4 KiB copy when the folio is shared), `thp_get_unmapped_area()` (2 MiB aligned placement). Zircon maps large pages only for physically contiguous VMOs and splits by allocating, unmapping the whole leaf if that fails (`ArchUnmapOptions::Enlarge`, `X86PageTableImpl::RemoveMapping()`); the user chose Linux's way instead.

## Alternatives considered
- Split by allocating, and unmap the whole leaf when that fails (Zircon): rejected by the user in favour of a split that cannot fail.
- Never split, always unmap the whole leaf: every partial change refaults up to 511 neighbours.
- Large leaves only for physically contiguous VMOs (Zircon): leaves ordinary memory, the case that needs TLB reach most, at 4 KiB.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. How a partial change to a 2 MiB leaf is done | split by allocating, unmap the whole leaf on failure (Zircon); split with a table deposited when the leaf was mapped (Linux); never split | deposit | None | deposit, 2026-10-24 |
| D2. Which VMOs get 2 MiB blocks | every eligible fault (Linux THP "always"); only VMOs created with a kernel-internal large-page flag (THP "madvise"), the default revisited after the measurement | the flag | None | |
| D3. 1 GiB user leaves | no; yes, with a deposit of one PD table and 512 PT tables each | no | None | |
| D4. Where deposits live | in the descriptor of the PD table page that holds the leaf, a FIFO through the deposited tables' list hooks (Linux); a side table per address space keyed by address | the descriptor | None | |
| D5. Large leaves in the kernel address space | unchanged: boot's, never split, a partial change stays InvArgs; deposits there too | unchanged | None | |
| D6. Where a large-page VMO is placed when the kernel picks the address | at an address congruent to its VMO offset modulo 2 MiB, as Linux's `thp_get_unmapped_area()` does; wherever the allocator puts it today | congruent | None | |
| D7. A write to a 2 MiB block a clone shares | split the writer's leaf and copy or take the one 4 KiB page (Linux `do_huge_pmd_wp_page()` fallback); copy the whole 2 MiB | the one page | None | |
| D8. Putting split pages back into a 2 MiB leaf | not now; a background collapse (Linux khugepaged) | not now | None | |

## Testing and verification
- Host tests of the engine (`kernel/lib/arch/test/paging_test.cpp`): a 2 MiB leaf mapped with a deposit; unmap and protect of part of it split it without calling the allocator, and the 4 KiB entries name the same frames with the right permissions; an unmap of the whole leaf and a teardown return every deposit; a leaf mapped with no deposit available falls back to 4 KiB.
- kshell: a large-page VMO faulted, read and written through a 2 MiB leaf (the query answers a 2 MiB size); a partial unmap, a protect, a clone's remove-write and a copy-on-write fault each split it and leave every page with its content; `take_pages()` of part of a block, once user-pager reaches it.
- `benches/`: random and sequential 8-byte reads over a 256 MiB VMO, with 4 KiB and with 2 MiB leaves; time per access, and TLB misses if the PMU sampler can count them. The numbers decide D2's default with the user.
- `ours-verify` on every step.

Cannot be verified: that the old 2 MiB translation is gone from another CPU's TLB at the moment of a split, except through the shootdown checks that exist for 4 KiB entries.

## Open questions
- Whether `vmo_create` should offer the large-page flag to programs, once D2 is settled by the measurement.
- Whether the PMU sampler can count DTLB misses on the QEMU CPU models the boot uses.
