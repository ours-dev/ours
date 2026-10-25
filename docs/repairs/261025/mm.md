# mm outstanding repairs

## 1. A zero marker put by hand over an ancestor's page leaves the page's viewer count high

**Severity**
Low: only tests call `add_zero_marker_locked()` on a node under a hidden one; nothing in the kernel does.

**Symptom**
Latent, cannot be triggered today. `VmCowPages::add_zero_marker_locked()` on a clone, at an offset where a hidden ancestor holds a page, hides the page from the clone without counting the clone out of the page's viewers. The count stays one too high, and when the hidden node is merged into its last child, `DEBUG_ASSERT(survivor.is_hidden() || 0 == page->share_count())` fires ("a page with another viewer in a hidden node with one child"). Found on 2026-10-25, when the first version of the take-from-clone check in `vmosnapshot.test` put such a marker and the kernel panicked at the end of the check.

**Cause**
A page in a hidden node counts how many leaves below it see it (`share_count()`). Everything that stops a leaf seeing one keeps the count: a write's copy (`cow_page()`), a leaf's death (`release_viewed_pages_locked()`), and `take_pages_locked()`, which only leaves a marker where the leaf already holds a page of its own, a copy made by `cow_page()`. `add_zero_marker_locked()` inserts a marker wherever it is told, and does not look up.

**Change**
Either drop the share of the page the marker hides (look it up with `shows_ancestor_page_locked()`'s walk, and `drop_share()` it, freeing it if this was the last viewer, as `release_viewed_pages_locked()` does), or refuse such an offset with a `DEBUG_ASSERT`, since no kernel path needs it. The second is enough until one does.

**Reference**
Zircon keeps the count the same way on every path that hides parent content (`VmCowPages::DecrementCowContentShareCount()`, called from `SupplyPagesLocked()`'s `handle_add_page_result` when a slot that showed a hidden ancestor's page is filled).

**Acceptance**
A `vmosnapshot.test` check that puts a marker on a clone over its hidden parent's page, destroys the clone's sibling, and finds the merge consistent (or, with the assert, that the debug build refuses the call).

## 2. A request withdrawn while others wait on it is sent to the pager again

**Severity**
Low: nothing is wrong, since a second supply of a page is given back, but the pager does work twice or a request loses its place in line. A performance item, so it goes through the user before it is fixed.

**Symptom**
Two threads fault on the same page of a pager VMO. The first one's request is the primary; the second waits on it as an overlap. The first thread's wait is then cut short by a kill, a suspend or the overtime TimeOut. If the pager has already read the first Read packet, it gets a second Read for the same range and supplies the range twice. If the packet was still on the port, it is taken back and queued again at the tail, behind every packet queued since.

**Cause**
`PageSource::cancel_request_locked()` hands a withdrawn primary's range to its first overlap, the heir. It tells the provider in two calls: `clear_async_request(old)`, then `send_async_request(heir)`. `PagerProxy` sees them as unrelated:

- `clear` takes `old` back if it can: off the port if still queued, or out of `m_pending`.
- `send` then sends `heir` as a new request: a fresh packet at the tail of the port, or the back of `m_pending`.

When the pager has already read `old`, there is nothing to take back, so `heir` goes out as a second Read for a range the pager is already supplying.

**Change**
Add `PageProvider::swap_async_request(old, heir)`, and call it from `cancel_request_locked()` in place of the two calls. `PagerProxy`'s version:

- `old` in `m_pending`: `heir` takes its place in the list.
- `old` is the packet's request (`m_active`): `m_active = heir`, and the packet stays where it is, since it already carries the range.
- Neither: nothing. The pager has the range and is working on it, and its supply resolves `heir`.

`page_source_test.cpp`'s TestProvider needs the method too.

Measure first: a `pager.test` case where the primary's thread is killed after the pager has read its packet, counting the Read packets the pager receives (2 today, 1 after). The cost being saved is one round trip through the pager per withdrawn primary with waiters, which only a kill, a suspend or a 300 s timeout causes.

**Reference**
Zircon `PageProvider::SwapAsyncRequest()` (`vm/include/vm/page_source.h`), `PagerProxy::SwapAsyncRequest()` (`object/pager_proxy.cc`), called from `PageSource::CancelRequestLocked()` (`vm/page_source.cc`).

**Acceptance**
`pager.test` passes two new checks:

- With the primary withdrawn after its packet was read, the port gets no second Read, and the overlap's thread wakes on the one supply.
- With the packet still queued, it keeps its place ahead of a packet queued after it.
