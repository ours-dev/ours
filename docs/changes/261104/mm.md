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
