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

## Encapsulate VMM flags and document their combinations (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/page_request.hpp` and `kernel/mm/page_request.cpp` encode PageSource lifecycle in private flags.
- `kernel/object/include/ours/object/pager_proxy.hpp` and `kernel/object/pager_proxy.cpp` put proxy flags under the existing mutex.
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` and `kernel/mm/vm_cow_pages.cpp` encode the cursor's accessed marking policy as a private flag.

**Symptom**

Stored pager and cursor flags were spread across boolean members. The PageSource lifecycle relationship was implicit in the methods that updated it.

**Cause**

The stored flags had individual declarations but shared lifecycle rules. The declarations did not describe which combinations the owner permitted.

**Change**

The private PageSource flag word starts with neither lifecycle bit set. Under the source lock, `detach()` sets Detached and `close()` replaces it with Closed. Both refuse new requests. PagerProxy keeps its packet and closure flags in one word under its own lock. The cursor starts with MarkAccessed and can clear it through its named method.

**Reference**

Zircon's `PageSource::Detach`, `PageSource::Close`, pager proxy packet bookkeeping and `VmCowPages::LookupCursor::DisableMarkAccessed` define the corresponding behaviors. The storage follows this repository's scoped flag convention.

**Verification**

Clang-format and LLVM 23 clang-tidy checked all six C++ files. Both QEMU dev images booted with all 163 host tests passing in both the default and four-node NUMA configurations. The targeted suites passed 373 checks in the default configuration and 132 checks in NUMA.

**Deliberately not done**

This change covers the representation and transitions of the reviewed flags.

**How it was found**

The user clarified that a private flag word can hold mutually exclusive bits when its owner enforces and documents the invariant.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the external VMO factory create_external (2026-11-04)

**Files**

- The paged VMO and COW node factories, pager dispatcher and their test callers.

**Symptom**

The `with_source()` name was inconsistent with the other VMO factory names.

**Cause**

The factory described its argument without naming the creation operation.

**Change**

`with_source()` becomes `create_external()` at both layers and at their call sites.

**Reference**

Zircon's `VmObjectPaged::CreateExternal` and `VmCowPages::CreateExternal`.

**Verification**

Clang-format and clang-tidy checked all nine C++ files. Both dev images booted and all 163 host tests passed in the default and four-node NUMA configurations. The targeted pager, page source, queue and reclaim suites passed 132 checks in default and 120 in NUMA.

**Deliberately not done**

The change covers the external factory name.

**How it was found**

The user asked to align this factory with the Zircon vocabulary.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
