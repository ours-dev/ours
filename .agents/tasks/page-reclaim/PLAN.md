# page-reclaim: steps from page queues nothing fills to a reclaimer the watchdog wakes

**Date**: 2026-10-30
**Status**: Accepted

## Request
The user, 2026-10-30: functionality before performance; VMO pages age, and when memory runs short pages a pager supplied are dropped and anonymous pages that are all zero become zero markers. Separate queues for each kind of page (D1), the software mark and the harvested accessed bit together (D2), reclaim only when the watchdog asks (D3), a fixed aging period (D4). Scope ends there: compression, swap and dirty pager pages are later.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Which pages age in the ring | see DESIGN.md | (a) | None (homework paused) | (b), 2026-10-30 |
| D2. Where recent use comes from first | see DESIGN.md | (b) | None | (a), 2026-10-30 |
| D3. When the reclaimer runs | see DESIGN.md | (a) | None | (a), 2026-10-30 |
| D4. The aging period | see DESIGN.md | (a) | None | (a), 2026-10-30 |

## Steps
Each step is introduced to the user before it starts, and lands in its own commit.

1. **Pages on the queues.** Every page a `VmCowPages` holds is filed on its node's queue for its kind (Pinned, the ring for a pager's pages, Anonymous, Anonymous zero-fork), with its backlink, wherever it enters a node's page list, moves between nodes, or leaves; the destructor takes its pages off under the queue lock. Why first: everything after reads the queues. Verified by a kshell test that compares each queue's pages with the VMOs that hold them, and controls. Done: `pagequeues.test`, 7 checks, on the default and the four-node build; a VMO destroyed without taking its pages off stops the boot on a new assertion in `free_frames()`, and a clone that does not hand its pages' backlinks to the hidden node fails the test. Also `VmCowPages::Flags::Pinned`, since the node files the pages and only the VMO knew it was pinned.
2. **Aging and the software mark.** The aging thread rotating the ring on its period (a boot option); `Cursor` marking a page it hands out. Verified by a test that ages a pager VMO's pages and sees a page marked go back to the newest generation.
3. **The harvest of the accessed bit.** A page-table walk that clears the bit and reports the pages it was set on, `ArchVmAspace::harvest_accessed()`, the TLB invalidation for it, and the aging thread harvesting before it rotates. Verified by a test that touches a page only through a mapping and sees it go back to the newest generation.
4. **Reclaiming pager pages.** `VmCowPages` reclaiming one page at an offset, the reclaimer thread, and the lifetime rule (`Rc::try_adopt()` under the queue lock). Verified by a test that ages and reclaims a pager VMO's pages and reads them back through the pager, and a stress of VMOs destroyed while reclaimed.
5. **Deduplicating zero pages.** The zero check, remove-write, check-again path for the zero-fork queue. Verified by a test of zero and written pages after a reclaim.
6. **The watchdog wakes the reclaimer.** At Warning and Critical with a target, and once synchronously at OutOfMemory before refusing. Verified by `mempressure.test` seeing free memory rise at Critical.
7. Later steps are introduced one at a time.

## Files
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp`, `kernel/mm/vm_cow_pages.cpp` — steps 1, 2, 4, 5
- `kernel/arch/x86/lib/page_tables/`, `kernel/arch/x86/mm/` — step 3
- `kernel/mm/memory_watchdog.cpp` — step 6
- a kshell test in `kernel/commands/` — every step

## Test plan
- Every step: `boot_check.py`, both images; `pagesource.test`, `pager.test`, `vmosnapshot.test`, `largepage.test`, `mempressure.test`.
- Each step's own test and controls, as above.
