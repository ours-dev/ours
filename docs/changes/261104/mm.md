# mm changes

## Rename the VMO mapping range operation (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/types.hpp` names the operation enum `RangeOp`.
- The VMO, COW and mapping classes propagate the operation through the `operate_range` family.
- The snapshot and large-page test callers use the same names.

**Symptom**

The range helper name suggested a change to the range itself. It applies an operation to the translations that cover the range.

**Cause**

The helper's name described the effect in general terms and the enum's name did not identify its role as the input operation.

**Change**

The COW and VMO entries now use `operate_range_locked(offset, len, RangeOp::Unmap)`. The same rename applies to the VMO offset projection and the mapping operation.

**Reference**

Zircon propagates the corresponding operations through `VmCowPages::RangeChangeUpdateLocked`.

**Verification**

Clang-format and LLVM 23 clang-tidy checked all eleven C++ files. The default and NUMA builds each passed both QEMU dev boots and all 163 host tests. The default snapshot, large-page, reclaim, pager and physical-mapping suites passed 241, 28, 21, 23 and 8 checks respectively. The NUMA snapshot and reclaim suites passed 241 and 21 checks.

**Deliberately not done**

This change covers the kernel's mapping operations. The discardable VMO protocol was evaluated separately.

**How it was found**

The user requested the new range operation names.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Share COW page commitment and pinning (2026-11-04)

**Files**

- `kernel/mm/vm_cow_pages.cpp` shares one owned-page walk between commit and pin.
- `kernel/mm/vm_object_paged.cpp` shares the wait, resume and error handling.
- The COW cursor returns directly from node-relative construction. Its callers now use the cursor value.

**Symptom**

Commit and pin each walked the same page range and each handled partial progress across waits. Every direct COW cursor caller had to account for a result that always contained a cursor.

**Cause**

The range operations combined acquiring an owned page with deciding what to do once acquired. The COW cursor inherited the fallible shape of the VMO entry even though the VMO owns slice bounds validation.

**Change**

One private `commit_range_locked` takes `CommitMode::Owned` or `CommitMode::Pin`. Its generic lambda specializes the per-page operation. The shared VMO loop waits outside the lock and resumes after the completed prefix. A pin failure undoes the pins taken by that call. Direct COW cursor construction returns `Cursor`, and the VMO entry validates and translates the slice range.

**Reference**

Zircon's `VmObjectPaged::CommitRangeInternal` also shares wait and progress tracking between commit and pin.

**Verification**

Clang-format and LLVM 23 clang-tidy checked all twelve changed C++ files. Both QEMU dev images booted in the default and four-node NUMA configurations with all 163 host tests passing in each. Nine default kernel suites passed 524 checks. Four NUMA kernel suites passed 366 checks. The suites include a commit that resumes across pager replies and a pin that rolls back after the next page reaches its pin limit.

**Deliberately not done**

This change covers range traversal, retry and cursor construction.

**How it was found**

The user requested consolidating COW interfaces that perform overlapping work.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
