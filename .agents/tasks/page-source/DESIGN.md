# page-source: pages that come from a provider, asked for and waited on

**Date**: 2026-10-23
**Status**: Accepted

## Problem
Every VMO today is anonymous: a page nobody has written is zero, and the kernel makes it on the spot. A VMO whose content lives elsewhere (a file, a user pager, a compressed store) needs a different answer to "nothing here": ask whoever has the content, put the faulting thread to sleep, and resume it once the page arrives. The fault path already has the shape for this (VmAspace::page_fault_internal() waits on `vmf.page_request` and retries on ShouldWait, and VmCowPages::Cursor already branches to create_read_request() when a node has a source), but PageSource, PageRequest and PageProvider are empty stubs, `VmCowPages::with_source()` is declared and never defined, and `VmObjectPaged::supply_pages()` is Unimplemented. `docs/repairs/260903/mm.md` item 3 records the gap.

## Goals and non-goals
Goals, step 1:

- PageProvider, PageSource and PageRequest in `kernel/mm`, READ requests only.
- Concurrent requests for the same pages are merged: the provider sees one request, every faulting thread waits on it.
- A paged VMO can be created with a source; a fault on an absent page raises a request and waits; supply_pages() inserts the pages and wakes the waiters.
- An in-kernel test provider and a kshell command, `pagesource.test`, that drive all of it.

Non-goals, step 1:

- No pager kernel object, no syscalls, no port packets: those are step 2.
- No DIRTY or WRITEBACK requests, no early wake, no SwapAsyncRequest.
- No COW children of a sourced VMO (create_child and clones refuse it); see Open questions.
- commit_range(), read()/write() and a Populate mapping of a sourced VMO do not wait yet: they return ShouldWait, or trip commit_range_locked()'s assertion. Recorded in `docs/repairs/`.

## Design
Objects:

- `PageProvider`: an interface, "who has the content". The source calls `send_async_request(req)` to hand it a request, `clear_async_request(req)` to take it back, `wait_on_event(event, suspendable)` to let it decide how a waiter sleeps, and `on_detach()` / `on_close()`. While a request is handed over (`provider_owned`), the provider may read its offset and length through static accessors, and nothing else may change them.
- `PageSource`: one per sourced VMO, holding the provider. It keeps the outstanding requests in an intrusive red-black set keyed by end offset, under its own mutex. It is ref-counted; the root VmCowPages node and every raised request hold a reference.
- `PageRequest`: lives on the faulting thread's stack (the fault path must not allocate). Fields: offset, len, pending size, completion status, an AutoUnsignal Event, a reference to its source, a list of overlapping requests that wait on it, and three hooks (the source's set, a primary's overlap list, the provider's queue).

Events, one read fault:

1. The cursor finds no page in a root node that has a source and calls `create_read_request()`, which trims the run to the pages actually absent and calls `PageSource::get_pages(offset, len, req)`.
2. `get_pages()` initialises `req`, and looks for an outstanding request that contains its start (`upper_bound(offset)` on end offsets). If there is one, `req` is trimmed to that request's end and joins its overlap list. Otherwise `req` is trimmed so that it stops where the next outstanding request starts, inserted into the set and sent to the provider. Either way it answers ShouldWait.
3. The fault path unwinds with ShouldWait, drops the VMO and aspace locks, and calls `req.wait()`, which asks the provider to wait on the request's event.
4. The provider (a kernel thread in the test) reads offset and length, makes the pages, and calls `VmObjectPaged::supply_pages()`.
5. supply_pages() takes the VMO lock, inserts each page into an empty slot (frees it where the slot is already full), and calls `PageSource::on_pages_supplied()` for each run of new pages, still under the VMO lock.
6. `on_pages_supplied()` subtracts the supplied length from every request it overlaps. A request whose pending size reaches zero is erased from the set, taken back from the provider, marked uninitialised, and its event and those of all its overlap waiters are signalled.
7. The waiter wakes and the fault loop retries; this time the cursor finds the page.

Other events:

- Failure: `on_pages_failed(offset, len, status)` sets the completion status of the requests it touches, then resolves them as above; `wait()` returns that status.
- Cancel: a request that stops being waited on (the PageRequest goes out of scope, or the wait is interrupted) is taken out. An overlap waiter just leaves its primary's list. A primary with waiters hands its place to the first waiter: the old one is cleared from the provider and the new one sent (clear then send, in place of a swap).
- Detach and close: `~VmCowPages` closes the source, which detaches it first: every outstanding request is completed, later `get_pages()` answers BadState, and so does a retried fault.

Locking:

- Lock order: VMO (hierarchy) mutex, then the PageSource mutex, then the provider's lock. get_pages() runs under the VMO lock; so does on_pages_supplied(), called from supply_pages().
- The PageSource mutex protects the set, `detached`/`closed`, and every field of an initialised request.
- The provider's lock protects its queue. A provider must copy what it needs out of a request while holding its own lock and must not touch the request after letting go: the faulting thread can complete or cancel it, and return, the moment the lock is free. It must not call supply_pages() with its own lock held (that would be provider lock, then VMO lock: the reverse order).

Reference: Zircon `kernel/vm/page_source.{h,cc}` (PageSource::PopulateRequestLocked, SendRequestToProviderLocked, ResolveRequestsLocked, CompleteRequestLocked, CancelRequestLocked), `VmCowPages::LookupCursor::ReadRequest`, `VmCowPages::SupplyPagesLocked`, and the `StubPageProvider` in `kernel/vm/unittests/test_helper.h` as the precedent for a test provider.

## Alternatives considered
- One provider request per faulting thread, no merging: simpler, but N threads touching one page make the provider read it N times, and the second supply has to be thrown away. Rejected by the user (decision 3).
- A list instead of a tree for the outstanding requests: fine at today's counts, but the lookup is "the request whose end is past this offset", which is what an ordered set answers in log time. Kept the tree; the intrusive set needs no allocation on the fault path.
- SwapAsyncRequest for cancelling a primary with waiters: it exists so that a user pager that has already received the request's packet is not sent a second one. With no packets yet, clear-then-send is equivalent. Deferred to step 2.
- Early wake (a waiter resumes once the first page of its range is in): a latency gain for sequential readers; the fault path asks for one page, so there is nothing to gain yet.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Which request mechanism | anonymous wait-for-memory; pager-style PageRequest/PageSource | pager-style, with PageProvider | None (homework paused) | pager-style plus PageProvider, 2026-10-23 |
| How step 1 is verified | in-kernel test provider and kshell command; wait for syscalls | test provider | None | test provider, 2026-10-23 |
| Request types | READ only; READ and DIRTY | READ only | None | READ only, 2026-10-23 |
| Concurrent requests for the same pages | merge (one provider request, many waiters); one request per waiter | merge | None | merge, 2026-10-23 |
| COW children of a sourced VMO | refuse in step 1; resolve through the root's source | refuse in step 1 | None | |

## Testing and verification
`pagesource.test` in `kernel/commands`, run from kshell on both dev images:

- PageSource alone, with a test provider and requests on the stack: a single request reaches the provider; a request whose start lies inside an outstanding one joins it and is trimmed to its end; a request that runs into a later one is trimmed to stop there; a partial supply leaves the request outstanding and a full one completes it and its waiters; a failure completes with the failure status; cancelling a primary with a waiter re-sends the waiter; detach completes everything and refuses new requests.
- Through the fault path: a sourced VMO mapped into a test address space; a read fault on an absent page sleeps until a pager thread supplies it, and reads the supplied bytes; three threads faulting on one page produce one provider request and all read the same bytes; a failed request fails the fault.

Then `ours-verify` (build, boot both images).

## Open questions
- COW children of a sourced VMO: refuse in step 1 (recommended), or have a lookup that reaches a sourced root raise the request against the root's source as Zircon does?
