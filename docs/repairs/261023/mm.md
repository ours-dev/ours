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

## 2. A copy or a fault asks a page source for one page at a time

**Severity**
Low: performance only, and nothing but `pagesource.test` makes a sourced VMO today.

**Symptom**
Latent, cannot be measured yet. Once a user pager exists, a `vmo_read` of N unsupplied pages costs N request, packet, supply and wake round trips in sequence: the next request is raised only after the previous one is answered, unless the pager supplies ahead on its own.

**Cause**
`copy_page_by_page()` and the fault path both call `require_page(..., 1, ...)`, so `Cursor::create_read_request()` caps every request at one page. Zircon's `VmObjectPaged::ReadWriteInternal()` asks for the rest of the range on a read (up to 256 pages on a write). Asking for more here is not a one-line change: a request completes only when all of it is supplied (`PageSource::resolve_requests_locked()`), so a large request without early wake would make a reader wait for the whole range.

**Change**
Performance work, so measured first and decided with the user item by item (CLAUDE.md): count round trips and time for a sequential read through a user pager, then weigh a larger request against early wake, which Zircon pairs it with.

**Reference**
Zircon `VmObjectPaged::ReadWriteInternal()` (`max_waitable_pages`), `PageSource::EarlyWakeRequestLocked()`.

**Acceptance**
A measurement of a sequential read through a user pager, before and after, recorded with the user's decision.

## 3. A read of a hole in a node with no relatives commits a page

**Severity**
Low: memory footprint only; what a reader sees is the same.

**Symptom**
A read fault or a `vmo_read` of a page nobody wrote, in an anonymous VMO with no parent, allocates a zeroed page and keeps it. A read-only walk over a sparse VMO commits every page it touches, and `nr_pages_locked()` counts them.

**Cause**
`VmCowPages::Cursor::require_read_page()` answers a hole with the shared zero page only for a node with a parent or a hidden one; a node with no relatives falls through to `require_owned_page()`. Its comment gives the reason as "the only answer that is safe until the fault path maps by |writable|", but every caller now does: `map_paged()` and the fault path take Write off a page that is not writable, and the copy paths only read it. Noticed while adding zero markers, 2026-10-23, whose read of a marker already answers the zero page in such a node.

**Change**
Answer the shared zero page, read-only, for a hole of a node with no relatives and no page source, as for a node with a parent. A footprint change, so measured first and decided with the user (CLAUDE.md): count the pages a sparse read commits before and after, and check that `vmosnapshot.test`'s "a read of its hole commits a zeroed page of its own, writable" case is updated deliberately, not just made to pass.

**Reference**
Zircon `VmCowPages::LookupCursor::RequireReadPage()`, which answers zero content with `vm_get_zero_page()` whatever the node's place in the tree (`CursorIsContentZero()`).

**Acceptance**
A read of a hole in a node with no relatives leaves `nr_pages_locked()` unchanged and answers the zero page, read-only; a later write gets a page of its own.
