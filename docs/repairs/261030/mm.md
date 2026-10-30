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
