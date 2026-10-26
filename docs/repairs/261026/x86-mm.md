# x86-mm outstanding repairs

## 1. No kernel test destroys an address space with a 2 MiB leaf still mapped

**Severity**
Low: a missing test. The code it would cover is tested on the host.

**Symptom**
When a user address space is destroyed with a 2 MiB leaf still mapped, `PageTable::destroy()` must give back the table set aside for that leaf (`X86PageTableImpl::free_tables()` calls `discard_all_tables()`), or it leaks one table per leaf. No kernel test reaches that: with the call removed and the kernel rebuilt (2026-10-26, step 5 of `.agents/tasks/x86-page-table-crtp/PLAN.md`), both images boot and `largepage.test` passes, because every address space it destroys has unmapped its 2 MiB leaves first. Only the host test `FreeTables.GivesEveryTableBackAndCountsTheLeaves` fails.

**Cause**
`large_page_test.cpp` checks `PageTableDeposits::nr_held()` after destroying its address space ("with the address space gone, every table set aside is back"), but by then the leaf it mapped has been unmapped whole, which already gave its table back.

**Change**
In `large_page_test.cpp`, map a 2 MiB leaf into a fresh user address space, destroy the address space without unmapping it, and check that `nr_held()` is back to what it was before the leaf.

**Reference**
None, this project's own design: the deposits are this kernel's (decision D1 of `.agents/tasks/large-pages`).

**Acceptance**
With `discard_all_tables()` removed from `free_below()`, `largepage.test` fails; with it, it passes.
