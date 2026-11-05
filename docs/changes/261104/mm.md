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

## Restore with_source and record the naming precedence (2026-11-04)

**Files**

- The paged VMO and COW node factories and their callers.
- The kernel design and C++ style skills.

**Symptom**

The previous factory rename replaced the established `with_source()` spelling with Zircon's name.

**Cause**

The rename treated the reference implementation's spelling as the project's first choice.

**Change**

Restore `with_source()` at both layers and at their call sites. Apply Rust naming conventions as the project's default and use Zircon as a secondary reference for names.

**Reference**

The user's naming decision on 2026-11-04.

**Verification**

The restored kernel sources match the tree before the rename. Clang-format and clang-tidy checked all nine C++ files. Both dev images, all 163 host tests and the targeted pager and reclaim suites passed in the default and four-node NUMA configurations.

**Deliberately not done**

The code change is scoped to restoring this factory name.

**How it was found**

The user asked to retain `with_source()` and specified which naming convention takes precedence.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Use one MapOptions word across mapping creation and installation (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` defines the modes and valid bit masks.
- `kernel/mm/vm_area.cpp` validates creation and dispatches immediate installation.
- `kernel/mm/vm_mapping.cpp` checks installation modes and enforces the seal on direct changes.
- The dispatcher, kernel mappings and tests use the shared options.

**Symptom**

Mapping creation and page-table installation had different options and both used None for different behavior. The old mapping-level Pinned option was recorded by the builder without being consumed.

**Cause**

The options mixed the choice of which pages to install with attributes of the mapping being created.

**Change**

One scoped bitmask selects Lazy, Resident or Commit with ModeMask. ValidMask rejects unknown bits. Resident and Commit are exclusive. Creation consumes Sealed and passes only the selected mode to immediate installation. The kernel stack uses Commit at creation. Persistent device and descriptor mappings use Resident with Sealed, which ordinary unmap and protect enforce.

**Reference**

Zircon separates mapping creation from page-table installation through `VmAddressRegion::CreateVmMapping` and `VmMapping::MapRange`. The shared bitmask follows the user's choice.

**Verification**

Clang-format and LLVM 23 clang-tidy checked 27 C++ files. Both QEMU dev images and all 163 host tests passed in the default and four-node NUMA configurations. The default kernel suites passed 475 checks and the NUMA suites passed 364. Regression checks cover invalid options, the absence of side effects on rejection, Commit with Sealed and direct operations on sealed mappings.

**Deliberately not done**

The public ABI continues to express immediate installation through MapRange. The new bitmask is a VMM interface.

**How it was found**

The user asked to combine the VMM mapping option types and use masks to validate the resulting word.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Route COW range notifications through the representative VMO (2026-11-04)

**Files**

- `kernel/mm/vm_cow_pages.cpp` sends the synchronous range notification.
- `kernel/mm/vm_object_paged.cpp` owns view registration, handoff and traversal.
- `kernel/commands/vmo_snapshot_test.cpp` verifies notifications after handoff.

**Symptom**

The COW node owned a list of attachments solely to forward range operations to the VMO layer.

**Cause**

The node owned both the page mutation and enumeration of the views that display its pages. The views and their mappings already belonged to the VMO layer.

**Change**

Each node holds a non-owning pointer to one representative VMO. That VMO holds the non-owning list of the other views over the node. Destruction hands the list to a surviving view under the shared lock, or clears the pointer when the last view leaves. Each view clips the original notification independently. COW waits for the synchronous propagation before moving or freeing pages.

**Reference**

Zircon's `VmCowPages::RangeChangeUpdateLocked`, `VmObjectPaged::RangeChangeUpdateLocked` and `VmObjectPaged::~VmObjectPaged`.

**Verification**

Clang-format and LLVM 23 clang-tidy checked all five C++ files. Both dev images booted and all 163 host tests passed in the default and four-node NUMA configurations. The kernel suites passed 459 checks in default and 395 in NUMA. The snapshot suite exercises a successor whose window excludes the target page and a second handoff.

**Deliberately not done**

The change is scoped to direct views of one COW node.

**How it was found**

The user chose to keep the COW node as the source of the notification and the VMO as the owner of its propagation.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Keep view range clipping local to propagation (2026-11-04)

**Files**

- `kernel/mm/vm_object_paged.cpp` shares clipping within the view traversal.
- `kernel/mm/include/ours/mm/vm_object_paged.hpp` drops the private method declaration.

**Symptom**

The VMO declared a separate method for a step used only within its view traversal.

**Cause**

The representative and the peer views both need to intersect the original range with their own window. Both call sites belong to the same operation.

**Change**

`operate_views_locked` replaces the two calls to `operate_cow_range_locked` with `notify(*this)` and `notify(view)`. The local lambda clips and translates the range for each view. The saturated end of the original range is computed once.

**Reference**

None. The representative and peer views are traversed together in this project.

**Verification**

Clang-format and LLVM 23 clang-tidy checked both changed C++ files. The debug and release images booted and 163 host tests passed in each of the default and four-node NUMA configurations. The targeted kernel suites passed 372 checks in default and 268 in NUMA, including 247 snapshot checks in each.

**Deliberately not done**

None.

**How it was found**

A symbol search found both uses of the private method inside `operate_views_locked`.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Use usize for COW range notifications (2026-11-04)

**Files**

- `kernel/mm/vm_cow_pages.cpp`, `vm_object_paged.cpp`, `vm_object.cpp`, `vm_mapping.cpp` and their headers carry the range through the notification.
- The snapshot and large-page suites use the same type in their notification helpers.

**Symptom**

Range notifications used fixed-width integers for object-relative byte offsets and lengths. VMO sizes and mapping offsets use `usize`.

**Cause**

The notification signatures retained the older `u64` spelling. The range describes bytes in an object, including bytes that may have no physical backing at the time of notification.

**Change**

The notification carries `usize offset, usize len` through COW, the VMO views and the mapping. The view window also stores its starting offset as `usize`. At both intersections, the end is computed as `offset + std::min(len, ~usize{0} - offset)`.

**Reference**

Zircon's `VmObjectPaged::RangeChangeUpdateLocked` similarly interprets its input as an object-relative range. The `usize` spelling follows this project's VMO convention.

**Verification**

Clang-format and LLVM 23 clang-tidy checked all ten C++ files. Both dev images booted and 163 host tests passed in each configuration. The targeted kernel suites passed 296 checks in default and 268 in four-node NUMA.

**Deliberately not done**

None.

**How it was found**

The user noticed the fixed-width type while tracing notification propagation.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the VMM range notification range_change (2026-11-04)

**Files**

- The COW, VMO and mapping methods in `kernel/mm` and their callers in the snapshot and large-page tests.

**Symptom**

The notification methods used the `operate_range` name.

**Cause**

The old name emphasized the operation applied to a range. These methods are the path that propagates a range change to page tables.

**Change**

`operate_range_locked` becomes `range_change_locked` in COW and VMO. The mapping endpoint is `range_change_locked_object`. `RangeOp` still describes what the notification requests.

**Reference**

Zircon's `VmObjectPaged::RangeChangeUpdateLocked` uses the same range-change vocabulary for the VMO notification.

**Verification**

Clang-format and clang-tidy checked the ten C++ files with no diagnostics on changed lines. The debug and release dev images booted. All 163 host tests passed, along with 247 snapshot checks and 28 large-page checks.

**Deliberately not done**

None.

**How it was found**

The user asked to restore the range-change naming.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Invalidate old mappings when COW replaces backing (2026-11-04)

**Files**

- `kernel/mm/vm_cow_pages.cpp` notifies the VMO group when it installs a copied frame or materializes a mapped zero page.
- `kernel/commands/vmo_snapshot_test.cpp` verifies that both aliases lose their old entries.

**Symptom**

A write fault changed the frame at one alias of a cloned VMO offset. The second alias kept its readable entry on the inherited frame.

**Cause**

The COW node installed new backing without notifying the mapping lists. The fault resolved its own address and direct writes had no faulting address at all.

**Change**

Successful frame replacement calls `range_change_locked(offset, PAGE_SIZE, RangeOp::Unmap)` under the shared tree lock. The representative VMO propagates it to the affected views. The same notification covers zero markers and holes that previously supplied the shared zero page. Taking ownership of the same frame does not require a translation change.

**Reference**

Zircon `VmCowPages::CloneCowPageLocked`, `CompleteAddPageLocked`, and `ForkMarkerLocked`.

**Verification**

Changed lines passed clang-format and clang-tidy. Both development images booted and 163 host tests passed. The snapshot, page source and reclaim suites passed 255, 76 and 21 checks.

**Deliberately not done**

None.

**How it was found**

QEMU reproduced different present PTEs for two aliases of one clone offset after a write fault.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Check the VMO window before registering a mapping (2026-11-04)

**Files**

- `kernel/mm/vm_area.cpp` validates the page-aligned object offset and normalized length before registration.
- `kernel/commands/vmo_snapshot_test.cpp` exercises rejected windows and reuse of the address in all modes.

**Symptom**

A Lazy mapping starting at the end of its VMO could register successfully and fail later at fault time.

**Cause**

Only the user dispatcher and the paged cursor validated object bounds. Lazy creation did not construct a cursor.

**Change**

The ordinary creation path requires an aligned offset and rejects `size > vmo_size || vmo_ofs > vmo_size - size` before linking. Boot reservation enters separately through its existing `reserve()` path.

**Reference**

Zircon `VmAddressRegion::CreateVmMapping` validates the offset and overflow at creation. The object bounds follow this repository's user mapping contract.

**Verification**

Changed lines passed clang-format and clang-tidy. Both development images booted and 163 host tests passed. Snapshot, physical VMO, large-page and sealed mapping suites passed 259, 6, 28 and 15 checks.

**Deliberately not done**

None.

**How it was found**

A QEMU probe attempted to register one page starting just beyond the last VMO page with Lazy.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Publish mappings only after registration succeeds (2026-11-04)

**Files**

- `kernel/mm/vm_area.cpp` owns child registration and removal.
- `kernel/mm/vm_mapping.cpp` propagates registration failure and activates after both registrations.
- `kernel/mm/vm_aspace.cpp` withdraws its own references on mapping teardown.
- `kernel/commands/vma_seal_test.cpp` exercises duplicate insertion and overlapping reservations.

**Symptom**

Failed insertion could return an active mapping or sub-area absent from address lookup. Mapping teardown also decremented an unused count that reservation never incremented.

**Cause**

Activation marked the node active before insertion, and both factories discarded insertion errors. The lifecycle bit shared a mutable word with the permission ceiling, which callers read without the address-space lock. Teardown also reached into two address-space indexes.

**Change**

`link_child_locked` returns the tree result. The factories release unpublished storage on failure. Mapping activation sets its lifecycle state after both registrations. The permission ceiling is immutable, and active checks that can race teardown run under the address-space lock. Removal checks object identity. The address space clears its own indexes when notified by teardown.

**Reference**

Zircon `VmMapping::ActivateLocked` and `VmAddressRegion::Activate`.

**Verification**

Changed lines passed clang-format and clang-tidy. Both development images booted and 163 host tests passed. Seal, snapshot and large-page suites passed 18, 259 and 28 checks. A duplicate exercises registration failure without exhausting kernel memory.

**Deliberately not done**

None.

**How it was found**

The ownership review followed the result from `VmaSet::insert` to both creation paths and traced references through destruction.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Enforce permanent residency in the COW node (2026-11-04)

**Files**

- `kernel/mm/vm_cow_pages.cpp` checks permanent residency before clone and transfer.
- `kernel/mm/vm_object_paged.cpp` validates and translates the view window.
- `kernel/commands/vmo_contiguous_test.cpp` exercises both direct COW entries.

**Symptom**

A permanently resident node refused frame removal only when the operation passed through its VMO.

**Cause**

The COW node knew its frames were permanently resident for queue classification. The VMO independently checked its construction flag before calling the COW mutations.

**Change**

The node checks `Flags::Pinned` before modifying its page tree. The VMO translates the request. Counted pins still prevent clone or removal according to their existing range contracts.

**Reference**

Zircon `VmCowPages::CreateCloneLocked` checks counted pins at the backing mutation. Permanent residency is this repository's additional node policy.

**Verification**

Changed lines passed clang-format and clang-tidy. Both development images and all 163 host tests passed. The contiguous, pin, page-source and snapshot suites passed 18, 28, 76 and 259 checks.

**Deliberately not done**

None.

**How it was found**

The ownership review traced the permanent flag through queue classification, clone and transfer.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
