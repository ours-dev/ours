# mm outstanding repairs

## 1. Committing or copying a VMO with a page source does not wait for its pages

**Severity**
Medium: wrong, but nothing commits or copies a sourced VMO today.

**Symptom**
Latent, cannot be triggered today. `VmObjectPaged::commit_range()` on a sourced VMO with a missing page trips `DEBUG_ASSERT(status != Status::ShouldWait)` in `VmCowPages::commit_range_locked()`. `read()`, `write()`, `read_user()` and `write_user()` return ShouldWait to their caller, which no syscall caller expects.

**Cause**
The cursor raises a read request and answers ShouldWait (`docs/changes/261023/mm.md`), and only the page fault path (`VmAspace::page_fault_internal()`) knows to drop its locks, wait on the request and come back. The commit and copy loops hold the VMO's lock and treat every error as final.

**Change**
In `commit_range_internal()` and `copy_page_by_page()`: on ShouldWait, leave the locked block, wait on the request with no lock held, and retry from the first page not yet done (`nr_committed` for the commit, `done` for the copy), rebuilding the cursor, as the copy loop already does for a user-buffer fault. The request must outlive the locked block, so it moves out of it.

**Reference**
Zircon `VmObjectPaged::CommitRangeInternal()` and `VmObjectPaged::ReadWriteInternalLocked()`, which wait on the page request with the lock dropped and retry.

**Acceptance**
A `pagesource.test` case that commits, and one that reads, a range of a sourced VMO while the test supplies the pages; both succeed after the supply.

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

## 3. A page request that times out dumps the address space without its lock

**Severity**
Medium: reached only when a provider's wait gives up, which only the test provider does, after five seconds.

**Symptom**
Latent, cannot be triggered today except by a failing `pagesource.test`. `page_fault_internal()` calls `m_root_vma->dump_locked()` when the wait answers TimeOut, after the guard on the address space's lock has gone out of scope.

**Cause**
The wait has to happen with no lock held, and the dump was written next to it. A `_locked` function called without its lock reads the tree of areas while another thread may be changing it.

**Change**
Take the address space's lock around the dump, or drop the dump: the provider is the one that knows why it timed out, and can report it in `wait_on_event()`.

**Reference**
None, this project's own design.

**Acceptance**
With the test provider's wait shortened and no supply made, the fault fails with TimeOut and the kernel takes no lock assertion.
