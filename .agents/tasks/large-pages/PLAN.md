# large-pages: the steps, one commit each

**Date**: 2026-10-24
**Status**: Proposed

## Request
"可是我们需要支持大页" … "先做大页，选B": user memory mapped with 2 MiB leaves, a leaf split with a page table deposited when it was mapped, done before user-pager goes on past its step 4.1. Ends at DESIGN.md's goals; each step is introduced to the user before it starts.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md D1 to D8 | | | | |

## Steps
1. arch: the paging engine maps a 2 MiB leaf with a deposit given by its caller, splits it with that deposit when an unmap or protect covers part of it, and hands deposits back on a whole-leaf unmap and on teardown; a leaf with no deposit available is mapped as 4 KiB entries. Host tests.
2. mm and x86: the deposit's home in the `Mmu` role's descriptor body (D4); `ArchVmAspace` and `PageTable` pass deposits for user address spaces only (D5), and invalidate the 2 MiB translation on a split. kshell: a physically contiguous run mapped into a user address space as a 2 MiB leaf, then unmapped and protected in part.
3. mm: VMO blocks. A kernel-internal large-page flag (D2); a fault or commit in an empty, 2 MiB aligned range of a root node with no source allocates an order-9 block, falling back to 4 KiB; the head records the block; a block is taken apart when any page of it leaves, is copied, or is freed alone.
4. mm: mappings use 2 MiB leaves. The fault path and `map_paged()` map a whole block as one leaf when the mapping and its protection cover it; a clone's remove-write, a protect and a copy-on-write fault split it (D7).
5. mm: a large-page VMO is placed at an address congruent to its offset modulo 2 MiB when the kernel picks the address (D6).
6. benches: the measurement, recorded with the user's call on D2's default.

## Files
- `kernel/lib/arch/include/arch/paging/mod.hpp`, `controls.hpp`, `kernel/lib/arch/test/paging_test.cpp` — step 1
- `kernel/mm/include/ours/mm/pm_frame.hpp`, `kernel/arch/x86/mm/page_table.cpp`, `arch_vm_aspace.cpp` and their headers — step 2
- `kernel/mm/vm_cow_pages.cpp`, `vm_object_paged.cpp` and their headers, `kernel/mm/include/ours/mm/vm_page_map.hpp` — step 3
- `kernel/mm/vm_mapping.cpp` — steps 4 and 5; `kernel/mm/vm_area.cpp` — step 5
- `kernel/commands/` tests, `benches/` — steps 2 to 6

## Test plan
As DESIGN.md, Testing and verification. Each step: `ours-verify` passes and the tests it adds pass.
