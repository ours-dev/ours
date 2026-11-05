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

## Exercise VMM allocation failures in an opt-in kernel (2026-11-05)

**Files**

- `kernel/mm/alloc_failure.cpp` and its private header implement one scoped failure on the owning test thread.
- The kernel collection resource, object cache, page table and VMO frame allocation supply the failure points.
- `kernel/commands/vma_seal_test.cpp` and `kernel/commands/vmo_snapshot_test.cpp` exercise initialization, registration, mapping and COW rollback.

**Symptom**

Normal QEMU tests could reproduce duplicate registration. They could not reliably make VMA tree growth, object allocation or page-table allocation fail at a specific step.

**Cause**

Allocation failures depend on memory and allocator state. Exhausting memory globally would also disrupt unrelated kernel threads and make the failure site nondeterministic.

**Change**

The optional `mm_test_alloc_failure=true` configuration enables a thread-owned scope with a selected allocation site and skip count. The scope fails one matching allocation and records that it fired. Tests assert both the error and the resulting mapping or backing state, then retry. The default configuration compiles the failure sites out.

**Reference**

None. The failure points use the repository's existing allocation contracts.

**Verification**

The default and opt-in configurations booted both dev images and passed 163 host tests each. LLVM 23 clang-format and clang-tidy checked the C++ changes, and the repository GN formatter checked the three build files. All 46 instrumented commands passed. The seal suite passed 45 checks and the snapshot suite passed 270. The opt-in VMM line result was 4234/4768 (88.80%). It takes 296 more covered lines to reach 95% in that build.

Run the fault cases with `python3 scripts/cov_kernel.py --out out/vmm-coverage` after configuring the output with `--variant coverage-kernel --args 'mm_test_alloc_failure=true'`. The injected page-table error is logged as an error before the test reports its passing verdict.

**Deliberately not done**

None.

**How it was found**

The VMM coverage gaps included rollback paths that could not be reached with the ordinary test workloads.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Exercise allocation failure while transferring pages (2026-11-05)

**Files**

- `kernel/lib/heap/src/new.cpp` provides the opt-in failure site for kernel new and new[].
- `kernel/commands/page_source_test.cpp`, `kernel/commands/vma_seal_test.cpp` and `kernel/commands/vmo_snapshot_test.cpp` exercise supply, protection and transfer rollback.

**Symptom**

An allocation error while preparing a range or transferring its pages could leave the operation partially applied. The tests could only check successful transfers and permission changes.

**Cause**

Range splits can allocate tree nodes. A COW fork can allocate both a frame and a page-list slot. A splice must reserve space to hold transferred frames before it takes them from the backing node.

**Change**

The tests fail allocation at these steps and verify the original PTE permissions, backing data or empty destination slot as appropriate. They then retry the operation. Raw heap failures can also exercise hierarchy allocation during anonymous mapping and reservation.

**Reference**

None. These are failure contracts of the existing operations.

**Verification**

Clang-format and clang-tidy checked the five changed C++ files. The default and opt-in configurations booted both dev images and passed all 163 host tests each. All 46 instrumented kernel commands passed. The VMM measured 4260/4768 covered lines (89.35%). The 95% target requires another 270 lines.

**Deliberately not done**

None.

**How it was found**

The coverage report showed gaps when the kernel allocates range metadata and copies or transfers page slots.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
