# mm outstanding repairs

## 1. Reading a page nobody has written commits a zeroed page in the object, where Zircon reads zero without committing

**Severity**
Low: costs memory, not correctness; reached by every `vmo_read` and `process_read_memory` of a never-written page.

**Symptom**
`VmObjectPaged::copy_page_by_page()` (`kernel/mm/vm_object_paged.cpp`) resolves a read of an absent page by committing a zeroed page, as a fault would. So a debugger's or profiler's `process_read_memory` of a process's untouched heap or stack costs that process a frame per page read, and `vmo_read` of a sparse VMO commits what it reads. Zircon reads such a page as zero without committing it (`VmCowPages::RequirePage` with zero-fork disabled, the shared zero page).

**Cause**
There is no shared zero page: a read that finds no page has nothing to read from but a page of its own. `VmoDispatcher::read_user()` and `ProcessDispatcher::read_memory()` check the user buffer first so that a call bound to fail commits nothing, but a call that succeeds still commits.

**Change**
A read of an absent page copies zeros out without installing a page: either through a shared, read-only zero page that the copy reads from, or by having the read path zero-fill the user range directly for an absent page. The fault path keeps committing, since a mapped page must exist.

**Reference**
Zircon `VmCowPages::LookupCursor::RequireReadPage()` and the shared zero page (`vm_page_list` marker, `vm::ZeroPage()`).

**Acceptance**
A process's committed pages (`InfoMaps::committed_pages`) of a never-written range stay 0 after another process reads that range with `process_read_memory`; a shell check of that; both images boot.
