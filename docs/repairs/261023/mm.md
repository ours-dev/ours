# mm outstanding repairs

## 1. Committing a VMO with a page source does not wait for its pages

**Severity**
Medium: wrong, but nothing commits a sourced VMO today, and no system call can commit a VMO.

**Symptom**
Latent, cannot be triggered today. `VmObjectPaged::commit_range()` on a sourced VMO with a missing page trips `DEBUG_ASSERT(status != Status::ShouldWait)` in `VmCowPages::commit_range_locked()`.

**Cause**
The cursor raises a read request and answers ShouldWait (`docs/changes/261023/mm.md`). The copy loop waits on it since 2026-10-23; the commit loop still treats every error as final, under the VMO's lock.

**Change**
In `commit_range_internal()`: on ShouldWait, leave the locked block, wait on the request with no lock held, and retry from the first page not yet committed (`nr_committed`), as `copy_page_by_page()` does. The request moves out of `commit_range_locked()` so that it outlives the lock.

**Reference**
Zircon `VmObjectPaged::CommitRangeInternal()`, which waits on the page request with the lock dropped and retries.

**Acceptance**
A `pagesource.test` case that commits a range of a sourced VMO while the test supplies the pages; the commit succeeds after the supply.

## 2. A Populate mapping of a VMO with a page source fails

**Severity**
Low: nothing maps a sourced VMO with `VmMapOption::Populate`.

**Symptom**
Latent, cannot be triggered today. `VmArea::map_with_vmo(..., VmMapOption::Populate, ...)` on a sourced VMO with any page missing fails with ShouldWait from `VmMapping::map_paged()`, and the mapping is undone.

**Cause**
`map_paged()` asks the cursor for every page of the window (`require_page()`), and for a missing page of a sourced VMO the answer is a request, not a page. Populating means "map what is there", which for a sourced VMO is the pages already supplied.

**Change**
Have `map_paged()` skip a page the VMO does not have rather than ask for it when the VMO has a source, so a populate maps the supplied pages and leaves the rest to fault.

**Reference**
Zircon `VmMapping::MapRange()`, which maps only the pages already present when it is not asked to commit.

**Acceptance**
A populate mapping of a half-supplied sourced VMO succeeds, a read of a supplied page does not fault, and a read of a missing page raises a request.

## 3. A copy or a fault asks a page source for one page at a time

**Severity**
Low: performance only, and nothing but `pagesource.test` makes a sourced VMO today.

**Symptom**
Latent, cannot be measured yet. Once a user pager exists, a `vmo_read` of N unsupplied pages costs N request, packet, supply and wake round trips in sequence: the next request is raised only after the previous one is answered, unless the pager supplies ahead on its own.

**Cause**
`copy_page_by_page()`, the fault path and `map_paged()` all call `require_page(..., 1, ...)`, so `Cursor::create_read_request()` caps every request at one page. Zircon's `VmObjectPaged::ReadWriteInternal()` asks for the rest of the range on a read (up to 256 pages on a write). Asking for more here is not a one-line change: a request completes only when all of it is supplied (`PageSource::resolve_requests_locked()`), so a large request without early wake would make a reader wait for the whole range.

**Change**
Performance work, so measured first and decided with the user item by item (CLAUDE.md): count round trips and time for a sequential read through a user pager, then weigh a larger request against early wake, which Zircon pairs it with.

**Reference**
Zircon `VmObjectPaged::ReadWriteInternal()` (`max_waitable_pages`), `PageSource::EarlyWakeRequestLocked()`.

**Acceptance**
A measurement of a sequential read through a user pager, before and after, recorded with the user's decision.
