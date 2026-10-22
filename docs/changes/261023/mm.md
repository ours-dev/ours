# mm changes

## Pages of a VMO can come from a provider: PageSource, PageRequest and PageProvider (2026-10-23)

**Files**
- `kernel/mm/include/ours/mm/page_request.hpp` — PageProvider (interface), PageRequest (on the faulting thread's stack), PageSource (the outstanding requests of one VMO)
- `kernel/mm/page_request.cpp` — raising, merging, resolving, failing, withdrawing and detaching requests
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `with_source()` defined, `Cursor::create_read_request()`, `supply_pages_locked()`, the source closed in `~VmCowPages`, children and clones of a sourced node refused
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_object_paged.cpp` — `create_with_source()`, `supply_pages()`
- `kernel/mm/vm_mapping.cpp`, `kernel/mm/vm_aspace.cpp` — comments that said ShouldWait could not happen
- `kernel/commands/page_source_test.cpp`, `kernel/commands/BUILD.gn` — `pagesource.test` and its test provider
- `docs/repairs/260903/mm.md` — item 3 removed; what is left is in `docs/repairs/261023/mm.md`

**Symptom**
None: a missing feature. Every VMO was anonymous. `PageSource` was an empty class, `PageRequest::wait()` and `create_read_request()` returned Unimplemented, `VmCowPages::with_source()` was declared and never defined, and `VmObjectPaged::supply_pages()` was Unimplemented. The fault path already waited and retried on ShouldWait, but nothing could answer it.

**Cause**
An anonymous VMO answers "nothing here" by allocating a zero page on the spot. A VMO whose content lives elsewhere (a file, a user pager) cannot: the kernel has to ask whoever has the content and put the faulting thread to sleep until it arrives. Four things make that harder than it sounds, and each one shaped the design.

First, the fault path must not allocate: an allocation can recurse into reclaim and fault again. So a request lives on the faulting thread's stack (`VmFault::page_request`, outside the retry loop), and every container it goes into is intrusive: the source's set of outstanding requests, a primary's list of waiters, the provider's queue.

Second, the faulting thread holds the address space's lock and the VMO's lock when it finds the page missing, and the provider needs the VMO's lock to supply. So the request is raised under the locks, the fault unwinds with ShouldWait, `page_fault_internal()` drops both locks, waits, and takes the whole fault again from the top, because anything may have changed while it slept.

Third, several threads can fault on the same page. One request per thread would make the provider produce the page several times and throw all but one away. The source merges them: a request whose first page is already asked for joins that request's waiter list and is cut to that request's end, and the provider never sees it. A request that runs into a later outstanding one is cut to stop where that one starts. So the provider is never asked for one page twice. The set is keyed on end offset because two primaries never overlap: the only one that can contain an offset is the first whose end is past it, which is one `upper_bound()` (hence the key lookup added to the ustl Set, `docs/changes/261023/ustl.md`).

Fourth, the lifetime of a request on someone else's stack. When the source completes a request it signals the waiter's event while still holding its own lock, and goes on touching the request and its waiters. That is safe because a woken waiter's next step on that object is `~PageRequest()`, or a retry's `get_pages()`, and both take the same lock: the memory cannot go before the completion lets go. The provider, which runs on its own thread, has no such guarantee, so it copies offset and length out under its own lock and never touches the request again.

Lock order is the VMO's lock, then the source's, then the provider's. `get_pages()` runs under the VMO's lock (from the cursor), and so does `on_pages_supplied()` (from `supply_pages_locked()`). The source calls the provider with its own lock held, so a provider must not call back into the source or the VMO from those methods.

**Change**
The read fault now goes:

```text
cursor finds no page, node has a source -> create_read_request() -> PageSource::get_pages()
  -> new primary: insert into the set, provider.send_async_request()     ShouldWait
  -> or: join the primary that already asks for this page                 ShouldWait
page_fault_internal() drops its locks -> PageRequest::wait() -> provider.wait_on_event()
provider -> VmObjectPaged::supply_pages() -> VmCowPages::supply_pages_locked()
  -> pages into empty slots -> PageSource::on_pages_supplied(run)
  -> every request whose pending size reaches 0: out of the set, provider.clear_async_request(),
     its event and its waiters' events signalled
waiter wakes -> the fault is taken again -> the cursor finds the page
```

`require_owned_page()` raised the request before; it used to end in `create_read_request()` returning Unimplemented:

```cpp
auto VmCowPages::Cursor::create_read_request(usize nr_pages, PageRequest *page_request) -> Status {
    return Status::Unimplemented;
}
```

It now asks for the run of absent pages starting at the cursor, no longer than asked, than the cursor's range or than the node:

```cpp
u64 len = PAGE_SIZE;
while (len < limit && !m_target->m_pages.get_page(PgOff((m_offset + len) >> PAGE_SHIFT))) {
    len += PAGE_SIZE;
}
return m_target->m_page_source->get_pages(m_offset, len, page_request);
```

A write fault on a missing page raises the same read request: it needs the content before it can write. There are no DIRTY requests.

Other events:

- A failure (`on_pages_failed()`) sets the status of every request whose first page it covers, then resolves them as a supply would; `wait()` returns that status and the fault fails. A failure further into a request is not its waiter's failure: the waiter needs only its first page.
- A withdrawn request (out of scope, or its wait was interrupted) leaves. A waiter just leaves its primary's list. A primary with waiters hands its place, range and all, to the first waiter, which the provider is sent in its stead.
- `~VmCowPages` closes the source, which detaches it first: every outstanding request is completed with Ok, the waiters retry, and the retry finds the source detached (BadState) or the mapping gone.
- `supply_pages()` with a slot already full frees the supplied page and does not count it; only runs of pages actually added are reported to the source.

Rejected: one provider request per waiter (the user chose merging); a list instead of the set (the lookup is ordered by nature, and the set needs no allocation); `SwapAsyncRequest` for handing a withdrawn primary to its waiter (it exists so a user pager that already received the request's packet is not sent another; with no packets, clear then send is the same); early wake (the fault path asks for one page, so there is nothing to wake early for).

**Reference**
Zircon `kernel/vm/page_source.cc`: `PageSource::PopulateRequestLocked()` (the cutting against outstanding requests), `SendRequestToProviderLocked()` (the overlap list), `ResolveRequestsLocked()` (pending size and failure on the first page), `CompleteRequestLocked()` (clear before signalling), `CancelRequestLocked()` (the waiter that takes over), `Detach()` and `Close()`. `VmCowPages::LookupCursor::ReadRequest()` for the run of absent pages, `VmCowPages::SupplyPagesLocked()` for reporting runs of new pages, `VmCowPages::DeadTransitionLocked()` for closing the source when the node dies, and `StubPageProvider` in `kernel/vm/unittests/test_helper.h` as the precedent for a provider that exists only for tests.

**Verification**
- `pagesource.test` on the dbg image: 32 checks pass. PageSource alone: cutting, merging, partial supply, failure, a withdrawn primary handed to its waiter, a waiter that leaves, close. Through the fault path, on a sourced VMO mapped into a test address space: a read fault sleeps until its page is supplied and reads `0x9a9e50c000000000`; three threads faulting on one page raise three requests, of which the provider is sent one, and all three read the same bytes; a failed page fails the fault (`InvArgs` from the user copy); a write fault asks for the content, and the page holds the supplied word and the written one (`0x9a9e50c000000003 0x123456789abcdef`); a page supplied twice wakes nobody; the VMO closes its source when it goes.
- `usercopy.test`, `vmosnapshot.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass.
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), host tests 146 passed.
- Not measured: anything about speed.

**Deliberately not done**
- A pager kernel object and syscalls (a user-space provider, port packets): step 2, not approved yet.
- DIRTY and WRITEBACK requests, early wake, `SwapAsyncRequest`: see Change.
- Children and clones of a sourced VMO: refused with NotSupported. A lookup from a child walks up and stops at the root without asking the root's source, so a child would read unsupplied pages as zero. Whether to support it is an open decision in `.agents/tasks/page-source/DESIGN.md`.
- `commit_range()`, `read()`/`write()` and a Populate mapping of a sourced VMO do not wait: `docs/repairs/261023/mm.md`.
- A provider-side check of which failure statuses are valid: there is no caller from user space yet to check.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
1. Why may the source signal a waiter's event and keep using the request afterwards, while the provider must not touch a request once it lets go of its own lock?
2. Thread A has an outstanding request for [0, 4P); thread B asks for [2P, 6P). What does B wait on, what does it ask for, and what happens to [4P, 6P)?
3. Why does the fault path drop both locks before waiting, and why does it take the fault again from the top rather than mapping the page it was waiting for?
