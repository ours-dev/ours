# page-reclaim: VMO pages age in their node's page queues, and when the watchdog says memory is short a reclaimer gives back all-zero anonymous pages and clean pages a pager supplied

**Date**: 2026-10-30

## Delivered
Every page a `VmCowPages` holds is filed on its node's `PageQueues` for its kind (pinned, the ring of eight generations for a page source's pages, anonymous, anonymous zero-fork), with a backlink to its owner. One thread per node rotates the ring every `kernel.mm.page-aging-seconds` (5 by default), after harvesting the accessed bit of every user mapping, and `Cursor` marks the pages it hands out. A reclaimer evicts the oldest pager pages and turns all-zero anonymous pages into zero markers, and the memory watchdog has it do so at Warning, Critical and OutOfMemory. The design is `DESIGN.md` here; the plan is `PLAN.md`.

- `b8cd808c` docs(task): the design, proposed.
- `ea2eaf1c` docs(task): D1 to D4 decided, the plan in six steps.
- `5bfc9533` feat(mm): step 1, the pages on the queues: `file_page_locked()` and its callers, the backlink moved with the page, `Flags::Pinned`, the anonymous zero-fork queue, `PageQueues::kind_of()`/`count()`/`set_owner()`, and `pagequeues.test`.
- `a9ef6023` feat(mm): step 2, aging: `PageQueues::start_runners(period)`, the thread, `rotate()`, `age_of()`, the boot option, and `Cursor::handed_out()` with `disable_mark_accessed()`. Closes `docs/repairs/261026/mm.md` item 9.
- `dec51303` feat(mm): step 3, the harvest: `X86PageTableImpl::harvest_accessed()`, `ArchVmAspace::harvest_accessed()` and `invalidate_all()`, `VmAspace::harvest_all_user_accessed()`, `PageQueues::harvest_accessed()`.
- `8ac3ebef`, `cc51e51b` refactor(mm): `VmAreaOrMapping::NodeEnumerator` and `descend()`, which the user asked for after step 3; `harvest_accessed()` and `for_each_node()` use it.
- `e391b531` feat(mm): step 4, pager pages: `PageQueues::reclaim()`, `VmCowPages::reclaim_page()`, the dirty rule, and `pagereclaim.test`.
- `c7cdf63a` feat(mm): step 5, zero pages: `PageQueues::dedup_zero_pages()`, `VmCowPages::dedup_zero_page()`.
- `53a68cda` feat(mm): step 6, the watchdog: `MemoryReclaimer`, and two calls in `MemoryWatchdog::look()`.
- `07138cc6` docs(task): two open questions, methods by pressure and where compression starts.
- Records: `docs/changes/261030/mm.md`, seven entries (the merge, then the six steps). Repair items: `docs/repairs/261030/mm.md` items 1 to 4.

## Verification
At the final state (`53a68cda`):

- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 151 binaries, the page-table one with three new `HarvestAccessed.*` tests.
- `pagequeues.test`: 12 checks. The queues by kind and their backlinks; the aging thread rotating within a period; a supplied page's age, and a read through the VMO making it the newest; a page read only through a mapping a generation younger than one not read (the harvest).
- `pagereclaim.test`: 18 checks, on the default and the four-node build. `reclaim()` evicts the oldest pages and no more; an evicted page is asked of the provider again and reads as before when supplied; a written page leaves the reclaimable queues and keeps its bytes; a page of a page source is mapped read-only in a writable mapping; an evicted or deduplicated page is out of the mapping; 400 VMOs dropped while another thread reclaims, about 400 of 1600 pages evicted, none lost; zero pages become markers and a write to a marker commits a page; a thread writing through a mapping 20000 times while another deduplicates loses none; at Critical the watchdog has the reclaimer give back what is asked, zero pages first (16 of 16 frames, no pager page), then pager pages when more is asked; at OutOfMemory the watchdog reclaims on its own thread first (`gave back 48 frames before refusing anything`) and refuses nothing.
- `mempressure.test`: ok, with the reclaimer paused for its watchdog checks, run before and after `pagereclaim.test` in one session. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test`, `pmnode.test`, `pmzone.test`, `vmaseal.test`, `vmawx.test`, `vmaphys.test` ok throughout. No lockdep report.
- Controls, each restored after: aging with the mark off or the thread not started (2 checks of `pagequeues.test`); the harvest without the shootdown or taking no page (the new check, every run); eviction without the unmap, or a write not filing the page dirty (3 checks); a zero page deduplicated without the unmap (1 check) or without taking Write first (1 check, in 1 run of 3: the race is a few microseconds wide, so that check is a probabilistic one); the watchdog without the OutOfMemory reclaim (1 check) or without the request (2 checks).
- clang-tidy: nothing on the lines changed; clang-format clean.
- Not measured: the cost of anything here (`docs/repairs/261030/mm.md` items 1 to 3). Not verified: the lifetime rule at the exact moment between `try_adopt()` and the owner's lock, which the stress makes likely and cannot prove; pages of one VMO on different nodes with the harvest.

## Deviations from the design
- **A page written is not reclaimable, and a page of a page source is mapped read-only.** The design says "clean" and lists dirty pages as a non-goal; nothing tracked that a page was written, so eviction would have lost writes. A write that reaches `require_owned_page()` files the page anonymous, and the read lookups of a node with a source return `writable = false` (`file_dirty_locked()`, `writable_here()`). Written pager pages stay in memory until there is a write-back (`docs/repairs/261030/mm.md` item 4).
- **The harvest flushes a whole address space once**, with a CR3 reload on each CPU running it (`ArchVmAspace::invalidate_all()`), where the design says it invalidates each cleared entry. A harvest clears bits all over a space, and naming them would be one `invlpg` each. Its cost is `docs/repairs/261030/mm.md` item 2.
- **`PageQueues::kind_of()` takes the queue lock**, changing step 1's code in step 2: the aging thread rewrites the queue byte under that lock alone, so reading it under the VMO's lock was a data race.
- **The reclaimer is a synchronous call in step 4 and a thread in step 6** (the user's choice, 2026-10-30), where the plan put the thread in step 4.
- **Zero pages are deduplicated in leaf nodes only** (the user's choice, 2026-10-30), which settles the design's first open question for the first version.
- **`descend()`**, not in the design: the user asked for the node walk to belong to the base class with a visitor interface.

## Found along the way
- **The queue byte was read without the lock the thread now writes under** (step 2, fixed, above).
- **Writes to a pager VMO were not tracked at all**, found reading `require_owned_page()` before building eviction (step 4, fixed with the dirty rule).
- **Citations of Zircon**: several function names written from memory in drafts of the change records did not exist (`SetReclaimDirty`, `GetCowWithReplaceLocked`, `PopUnswappableZeroFork` as a name for the evictor's path); each was grepped against the Fuchsia tree and corrected before its commit, and what could not be verified is written as not traced.
- **The user's own change** to `VmAspace::switch_aspace()` (a return when the address space does not change, `198075b0`) turns a switch from null to null from a panic into a silent return; the user was told.

## Not done
- **Compression, swap and write-back of dirty pager pages**: non-goals of the design. Dirty pager pages are `docs/repairs/261030/mm.md` item 4, with a pager operation to drop a range.
- **Methods that differ by pressure, and where compression starts**: open questions of `DESIGN.md` (`07138cc6`), the second needing anonymous pages to have an age, which changes D1.
- **Zero pages in a hidden node**: an open question of `DESIGN.md`.
- **The cost of the mark, the harvest and reclaiming** (the queue lock per page handed out, the whole-space flush each period, locks and refetches against the VMO's users): `docs/repairs/261030/mm.md` items 1 to 3, each to be measured and put to the user.
- **Harvesting the kernel's address space**: the physmap's leaves are large, every kernel access sets them, and clearing a global entry needs a shootdown (the rule of 2026-10-25 in `ours-kernel-design`).
