# page-reclaim: VMO pages age in their node's page queues, and a reclaimer gives back pager pages and zero pages when memory runs short

**Date**: 2026-10-30
**Status**: Accepted

## Problem
The memory watchdog (`docs/design/261029/memory-pressure.md`) can only refuse: at OutOfMemory the allocations that could wait fail, because nothing can be given back. The pieces for giving memory back exist and are not connected:

- `PageQueues` (`kernel/mm/include/ours/mm/page_queues.hpp`), one per node, has a ring of 8 generations and a rotation, but no VMO page is ever filed into it and nothing rotates it (`docs/repairs/261026/mm.md` item 9).
- How a page's recent use is recorded was decided on 2026-10-25 (`ours-kernel-design`, references/mm.md, "Page aging"): the hardware's accessed bit, harvested from the page tables, and a software mark where `VmCowPages::Cursor` hands a page out. Neither is built; `ArchVmAspace::harvest_accessed()` is declared and never defined.
- The frame descriptor already has the backlink a reclaimer needs (`VmmBody::vmo`, `vmo_index`) and the byte that says which queue holds the page.

The user, 2026-10-30: functionality before performance; reclaim two kinds of pages first, (a) clean pages a pager supplied, which can be dropped and asked for again, and (b) anonymous pages whose content is all zero, which can become zero markers. Compression of anonymous pages is later.

What forces it: the user's direction. Honestly: (a) has no user outside tests today (no user-space file system creates pager VMOs), so its first effect is in tests; (b) has an effect on any process with pages it zeroed and never wrote.

## Goals and non-goals
Goals:

- Every page a `VmCowPages` holds is filed in its node's `PageQueues` with a backlink to its node and offset, and taken off when it leaves; a pinned VMO's pages on the pinned queue, never reclaimed.
- Pages age: a thread rotates the generations on a period, and a page used again goes back to the newest generation, by the software mark and by the harvested accessed bit.
- A reclaimer takes pages from the oldest generation: a pager page is unmapped everywhere, dropped from its node and freed, and the next fault asks the pager again; an anonymous page that is all zero becomes a zero marker and is freed; any other page is left.
- The watchdog wakes the reclaimer at Warning and Critical, and tries it synchronously at OutOfMemory before refusing.

Non-goals:

- Compression, swap, and dirty pager pages (writeback): later.
- Collapsing pages back into 2 MiB blocks.
- Per-process accounting and choosing which process loses memory.

## Design
Objects:

- **The queues (exist, one to add).** Per node, a queue for each kind of page (D1): Pinned, never reclaimed; the ring of 8 generations, for pages a pager supplied; Anonymous; and, new, Anonymous zero-fork, for anonymous pages committed in place of zero content (a write to a hole or a zero marker), the ones most likely to be zero again and the only ones the zero check looks at. A page is on one list at most, by the descriptor's single hook.
- **The backlink (exists, not written).** `VmmBody::vmo` is the owning `VmCowPages`, `vmo_index` the page's index in it. Written whenever a page enters a node's page list, rewritten when a page moves to another node (a hidden node taking a clone's pages, a supply, a take), cleared when it leaves.
- **The aging thread (new).** Every period, a boot option of 5 seconds by default (D4), harvests the accessed bits and then rotates the ring: the newest generation becomes one older, which ages every page at once without touching a descriptor; when the ring is full the oldest generation is folded into the next.
- **The access record (D2, both at once).** A software mark where `Cursor` hands a page out, as decided on 2026-10-25; and the harvest: for every user address space, a walk of its paged mappings' entries that clears each set accessed bit, invalidates that entry in the TLBs that may hold it (a cached translation would not set the bit again), and moves the page to the newest generation.
- **The reclaimer (new).** A thread woken by the watchdog with a target number of frames (D3). It takes pager pages from the ring's oldest generation and anonymous pages from the zero-fork queue's oldest end, gets each page's owner, and asks the owner to reclaim it at that offset; the owner decides, under its own lock, whether the page is still there and what to do with it. A zero-fork page found not zero moves to Anonymous, so it is not looked at again.

The lifetime rule, which is the delicate part: the reclaimer finds a page on a queue and has only the raw owner pointer from the backlink, while another thread may be dropping the owner's last reference. So:

1. The reclaimer holds the queue lock while it reads the backlink and calls `Rc::try_adopt()` on the owner, which gives an empty pointer if the owner has lost its last reference.
2. An owner's destructor takes every page it holds off its queue, under the same queue lock, before its storage goes. So while the reclaimer holds the queue lock, the owner's storage is still there, which is the condition `try_adopt()` states.
3. With a strong reference, the reclaimer lets go of the queue lock, takes the owner's lock, and checks that the page is still at that offset before it does anything; the page may have moved or gone while no lock was held.

Lock order: the owner's lock, then the queue lock (filing happens under the owner's lock). The reclaimer never holds the queue lock while it takes the owner's lock.

Reclaiming one page, under the owner's lock:

- A pager page, clean, not pinned: unmapped from every mapping of every object over it (`change_range_locked()` with Unmap, which `take_pages_locked()` already uses), removed from the page list, freed. The slot is a hole again, so the next fault raises a read request and the pager supplies it once more.
- An anonymous page, not pinned: checked for all zeros without touching the mappings; if zero, its mappings lose Write, it is checked again (a writer may have raced the first check), and if still zero it becomes a zero marker, its mappings are removed, and it is freed. A read then maps the shared zero page, a write commits a new page.
- Anything else stays where it is.

Reference: Zircon `PageQueues` (`zircon/kernel/vm/include/vm/page_queues.h`, `kNumReclaim = 8`, `RotateReclaimQueues()`), `ArchVmAspace::HarvestAccessed()`, `Evictor`, `VmCowPages::ReclaimPage()` with its `EvictionAction`, and `VmCowPages::DedupZeroPage()` (`zircon/kernel/vm/vm_cow_pages.cc:1363`), whose check, remove-write, check-again order the zero path follows.

## Alternatives considered
- **Reclaim inside the allocator**, when an allocation fails: the allocator would call up into VMOs; rejected for the PMM in `6794ed44`, and the watchdog already says when memory is short.
- **A per-node list of VMOs to scan** instead of per-page queues: reclaim would have to look at every page of every VMO to find old ones; the queues already order pages by age.
- **Compression first**: the largest gain, and the largest piece of work; after these two, when there is a measurement of how much (b) gives back.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Which pages age in the ring | (a) pager pages and anonymous pages in the one ring, the reclaimer deciding per page; (b) separate queues for each kind, as Zircon keeps | (a): one ring, one aging, and the reclaimer already has to look at each page; an anonymous page that is not zero goes back to the newest generation so it is not looked at again at once | None (homework paused) | (b), 2026-10-30 |
| D2. Where recent use comes from first | (a) the software mark and the harvested accessed bit together, as decided on 2026-10-25; (b) the software mark first, the harvest in a later step | (b): the harvest needs a page-table walk that clears bits and a TLB invalidation for them, a step of its own; until then a page used only through a mapping looks unused, which costs a refetch or a needless zero check, never wrong content | None | (a), 2026-10-30 |
| D3. When the reclaimer runs | (a) only when the watchdog asks, at Warning and above, and once synchronously at OutOfMemory; (b) also continuously in the background | (a): reclaim only when memory is short | None | (a), 2026-10-30 |
| D4. The aging period | (a) a fixed period, a boot option (5 seconds by default); (b) faster when the oldest generations are empty | (a) | None | (a), 2026-10-30 (first answered (b), changed to (a) the same day) |
| D5. Which methods run at which pressure | (a) the same at every pressure, as built; (b) a ladder by how costly a method is to undo, with an age floor that falls as the pressure rises: Warning gives back zero pages only, Critical adds pager pages and compression, OutOfMemory everything; (c) as built, with compression added at Critical only | (b) | None | (c), 2026-10-30: the other pressures stay as they are, and compression, when it is built, runs at Critical |

## Testing and verification
- A kshell test with a test pager: a VMO supplied, its pages aged by forcing rotations, a reclaim of the oldest generation; the pages gone from the VMO and its mapping, and an access asking the pager again and reading the same bytes.
- The same test with an anonymous VMO: zero pages committed and pages written; after a reclaim, the zero pages are markers and freed, the written ones are still there, every byte reads as before.
- A test of the lifetime rule: a VMO destroyed while the reclaimer is told to look at its pages, many times.
- `mempressure.test` extended: at Critical, the reclaimer runs and the free count rises.
- `boot_check.py` for both images; `pmnode.test`, `pmzone.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`.
- Not verifiable here: how much (b) gives back on a real workload; there is none yet.

## Open questions
- Whether a hidden node's zero pages, shared by several clones, are deduplicated in the first version: a zero marker in the hidden node reads as zero to every clone below it, but each clone that sees the page through it must lose its mappings of it too. A hidden node never holds a pager page: copy-on-write children of a pager VMO are still refused (`.agents/tasks/user-pager/RESULT.md`).
- Compression of anonymous pages (later work), whose trigger D5 decides: the request made at Critical, after pager pages, and not at Warning or at OutOfMemory. Not Warning, since it spends CPU on pages that may be hot and a read of one must decompress it. Not OutOfMemory, since compressing needs memory to make memory, an output buffer before the original page is freed, when free memory is already below the OutOfMemory level, and since the OutOfMemory reclaim runs on the watchdog's thread, which would wait for the slowest method. What is still open, for when it is built: it needs anonymous pages to have an age (one Anonymous queue, not harvested, and `mark_accessed()` moves only the ring's pages), so that cold ones can be told, which changes D1; `MemoryReclaimer::request()` does not say the pressure, and will have to; the reserve its output needs at Critical; and a page that does not compress is skipped.
