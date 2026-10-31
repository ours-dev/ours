# mm outstanding repairs

## 1. Every page a `VmCowPages::Cursor` hands out takes its node's queue spinlock, whatever kind of page it is

**Severity**
Low. A performance item, not a fault: nothing is wrong, and nothing has been measured.

**Symptom**
Latent, cannot be measured today: no workload reads VMOs heavily. `vmo_read()`, `vmo_write()`, a user copy's soft fault and a commit each take the page's node's `PageQueues` spinlock, with interrupts off, once per page, through `PageQueues::mark_accessed()`; on a node with many CPUs reading VMOs at once that one lock is shared by all of them.

**Cause**
The software mark (`VmCowPages::Cursor::handed_out()`, `.agents/tasks/page-reclaim` step 2) calls `mark_accessed()` for every page but the shared zero page, and `mark_accessed()` decides under the queue lock whether there is anything to do. Most pages there is not: an anonymous or pinned page has no age, and a reclaimable page already in the newest generation stays. Deciding without the lock is not possible as the queue byte is stored: it is a plain `u8` in `page::VmmBody` (`pm_frame.hpp`), and the aging thread rewrites it under the queue lock alone when it folds the oldest generation (`advance_lru_locked()`), so an unlocked read races that write, which is undefined behaviour however harmless a byte read looks.

**Change**
Measure first: a benchmark in `benches/` of `vmo_read()` of one page from 1, 2, 4 and 8 CPUs, one VMO per CPU, with time held and contention on the queue lock, against the same with the mark off. Then, if the lock shows, the candidate: make the queue byte atomic (`std::atomic_ref<u8>`, relaxed) and let `mark_accessed()` return without the lock when the byte is not a reclaimable generation or is already the newest, taking the lock only to move a page. Ruled out: moving the page without the lock by a compare-exchange on the byte alone, leaving it on the old list for the aging pass to reconcile, which buys the most and breaks the invariant `dequeue_locked()` trusts, that the byte names the list holding the page.

**Reference**
Zircon `PageQueues::MarkAccessed()` (`zircon/kernel/vm/page_queues.cc`), which reads the page's queue reference atomically, returns at once for a page not in a reclaim queue or already in the newest one, and otherwise moves the generation by compare-exchange, leaving the list to `ProcessLruQueues()` to correct.

**Acceptance**
A report in `docs/perf/` with the samples of the benchmark above before and after, and the decision the user takes from it.

## 2. The harvest clears the accessed bit of every page of a paged mapping, though only a reclaimable page uses the answer, and flushes the whole address space for it

**Severity**
Low. A performance item, not a fault: nothing is wrong, and nothing has been measured.

**Symptom**
Latent, cannot be measured today: no workload has a large user address space. Once a period, for every user address space that has a paged mapping, the aging thread walks every entry of every paged mapping, clears each set accessed bit it is asked about, and then sends an interrupt to every CPU running the space to drop the whole space's translations (`ArchVmAspace::invalidate_all()`). Every CPU then refills its TLB from the page tables on the next access, hot pages included.

**Cause**
`PageQueues::take_accessed()` answers true for any page of its node but the shared zero page, and `mark_accessed()` only moves a reclaimable page, so for an anonymous or pinned page the bit is cleared, the shootdown is paid, and the answer is thrown away. The walk visits a mapping's whole range, present or not; the flush is a full CR3 reload because no PCID is allocated and one reload per CPU is cheaper than naming the cleared pages. Which of these costs matters depends on how many pages are mapped and how many CPUs run the space, neither of which the tests have.

**Change**
Measure first: a benchmark in `benches/` that maps N pages (1 MiB to 1 GiB) in a space run by 1, 2, 4 and 8 CPUs, touches a fraction of them, and times `PageQueues::harvest_accessed()`, with the time the other CPUs lose to refills. Then the candidates, each with its cost: answer true only for reclaimable pages, so the bits of anonymous pages stay and cost no shootdown (but the zero-fork pages of step 5 may want theirs); skip a space that has not run since its last harvest, as Zircon does with `AccessedSinceLastCheck()`; flush by page, with `invlpg`, when few bits were cleared. Ruled out: not flushing (Linux `ptep_clear_flush_young()` on x86), which was decided against on 2026-10-30 for accuracy first.

**Reference**
Zircon `VmAspace::HarvestAllUserAccessedBits()`, which skips an address space not active since its last harvest, and `X86ArchVmAspace::AccessedSinceLastCheck()`.

**Acceptance**
A report in `docs/perf/` with the samples of the benchmark above before and after each candidate, and the decision the user takes from it.

## 3. A reclaimer and the VMOs it reclaims from contend on locks and on pages

**Severity**
Low. A performance item, not a fault: the re-check under the owner's lock keeps it correct, and nothing has been measured.

**Symptom**
Latent, cannot be measured today: no workload keeps a node's memory short for long. Reclaiming a page takes the node's queue spin lock to choose it and the owner's mutex to evict it, and the owner's reads, writes and faults take the same mutex, so a VMO that is being reclaimed from waits for the page being evicted. The page evicted is one the VMO may want again soon, and then it faults and is supplied again.

**Cause**
`PageQueues::oldest_reclaimable()` scans the oldest generation's list under the queue lock and takes the first page whose owner can be held; `VmCowPages::reclaim_page()` and `dedup_zero_page()` hold the owner's tree-wide mutex for the unmap, which is a shootdown to every CPU running a space that maps it. Age ranks pages but cannot tell one the VMO is about to use again.

**Change**
Measure first, with a benchmark in `benches/`: a reader of a pager VMO's pages from 1, 2, 4 and 8 CPUs while the reclaimer evicts from it, with the readers' latency distribution, the time the owner's lock is held per eviction and how often an evicted page is refetched within a second. Candidates, each with its cost: take pages in batches (one lock round trip for several pages of one owner); skip an owner whose lock is held (`try_lock`), at the cost of reclaiming less when everything is busy; an age floor, so that pages younger than some generation are left. Ruled out for now: reclaiming in the background all the time, which D3 of `.agents/tasks/page-reclaim/DESIGN.md` decided against.

**Reference**
Zircon `Evictor` (`zircon/kernel/vm/evictor.cc`), which takes its candidates from `PageQueues::PeekIsolate()` (line 58); whether and how it batches them was not traced.

**Acceptance**
A report in `docs/perf/` with the samples of the benchmark above before and after each candidate, and the decision the user takes from it.

## 4. A page source cannot invalidate pages it supplied, and a pager page that is written stays in memory for good

**Severity**
Medium. Wrong content, or memory that never comes back, but neither is reachable today: no user-space file system creates a pager VMO.

**Symptom**
Latent, cannot be triggered today. Once one does: a file changed on the source's side is seen through the VMO as it was when the page was supplied, until the page is evicted by pressure and supplied again. And a page written through the VMO is never evicted, since there is nowhere to write it back: a program that writes through a pager VMO pins that much memory.

**Cause**
The pager interface has `supply` and `fail` (`.agents/tasks/user-pager/RESULT.md`) and no operation that drops a range, and nothing writes dirty pages back (`file_dirty_locked()` files a written page anonymous, which is what keeps eviction from losing the write). A pager page costs no write when it is dropped and one fetch when it is needed again, which is cheaper than a swap-out; a written one has no copy anywhere else.

**Change**
Two pieces of work, in this order: an operation on a pager VMO that drops the pages of a range, so that the source can say a file changed (the page queues can already do the dropping, `reclaim_page()`; what is missing is the call and its rights); then dirty tracking with a request to the source to write a page back before it is evicted (`Cursor::create_dirty_request()` is declared and not defined). Each is a design for the user to decide, with the pager's interface in front of them.

**Reference**
Zircon `zx_pager_op_range()` with `ZX_PAGER_OP_FAIL`, `ZX_PAGER_OP_DIRTY` and `ZX_PAGER_OP_WRITEBACK_BEGIN`/`END`, and `PageSource::RequestDirtyTransition()`.

**Acceptance**
A kshell test with a test pager: a page dropped by the source's call is supplied again with the new bytes on the next access; a page written, written back and then evicted is read back with what was written.
