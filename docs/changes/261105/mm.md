# mm changes

## Extend VMM tests around failed operations and cleanup (2026-11-05)

**Files**

- `kernel/commands/vma_seal_test.cpp` checks duplicate registration, stale handles and cached lookups, protection preflight, and vDSO registration withdrawal.
- `kernel/commands/page_source_test.cpp` checks failed immediate mapping and the timeout and retry of a pager-backed pin.
- `kernel/commands/vma_physical_test.cpp` checks byte offsets, physical mapping permissions and preservation of the cache attribute.
- `kernel/commands/vmo_snapshot_test.cpp` checks rejection of accesses beyond a slice window.
- `kernel/mm/tests/protection_ranges_smoke.cpp` injects allocation failure through the existing host memory resource.

**Symptom**

The normal tests exercised successful mutations more often than rejection, rollback and retained references to destroyed objects.

**Cause**

An operation's intermediate state is visible only when a step fails after earlier work. A pager wait and a fallible tree insertion give deterministic ways to exercise those boundaries.

**Change**

The tests verify that `pin_range` releases an already pinned prefix after a timeout, then completes after supply and holds each page once. Failed activation leaves the original mapping intact. A rejected range leaves the slice and user buffer data unchanged. A host allocator that returns OutOfMemory verifies that failed protection splits preserve every page's permissions and can be retried.

**Reference**

None. The tests exercise the local interfaces and their existing contracts.

**Verification**

Clang-format and clang-tidy checked the five changed C++ files. Both dev images booted, all 163 host tests and 46 instrumented kernel commands passed. The four targeted suites passed 264 snapshot, 34 sealed-mapping, 81 page-source and 11 physical VMO checks. The host tracker reached 113/117 lines (96.58%) and 44/46 branches (95.65%).

The VMM scope covers instrumented `kernel/mm` files named `vm_*`, `page_*`, `protection_ranges.*`, `init_vmm.*`, `vmm.*` and `stack.*`. Kernel instrumentation covered 4173/4763 lines (87.61%) and 1714/2246 branches (76.31%). All 70 executable lines changed by the four repairs were covered. The overall line target of 95% still requires 352 additional lines. The raw coverage report is in `out/vmm-coverage/coverage/vmm-report.txt`.

**Deliberately not done**

None.

**How it was found**

The source-coverage report identified gaps in registration failure, retained mapping operations, pager pin rollback and protection coalescing.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
