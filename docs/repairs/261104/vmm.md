# vmm outstanding repairs

## 1. A COW fork leaves a second mapping on the old frame

**Severity**

High. Two mappings of the same VMO can read from different frames at the same object offset after the clone forks a page.

**Symptom**

A temporary QEMU probe extended `vmosnapshot.test` with two virtual addresses for the same clone offset. Both initially mapped the inherited frame. A write fault at the first address replaced its PTE with frame `0x141000`, while the second PTE remained present on the inherited frame `0x1f7000`.

**Cause**

`VmCowPages::cow_page` installs the new frame in the node at `kernel/mm/vm_cow_pages.cpp:1151` without notifying the VMO. The write-fault path in `VmMapping::widen_present_locked` only refreshes the address that faulted. A VMO write can reach the same transition without a mapping fault.

**Change**

Have the COW transition notify its VMO group synchronously through `range_change_locked` when backing changes. Audit zero-marker replacement for the same condition. The existing VMO and mapping interfaces already handle propagation.

**Reference**

Zircon `VmCowPages::CloneCowPageLocked`, `CompleteAddPageLocked` and `ForkMarkerLocked`.

**Acceptance**

Two prefaulted aliases of one clone offset see the same backing after either a mapped write or a direct VMO write. The other snapshot continues to see the original contents. Include the zero-marker transition in the regression.

## 2. A lazy kernel mapping can describe a window beyond its VMO

**Severity**

Medium. The user dispatcher validates the window. The kernel entry can register a range that will later fail at fault time.

**Symptom**

A temporary QEMU probe asked `VmArea::map_with_vmo` for a one-page Lazy mapping starting at the end of a two-page VMO. It did not reject the request with `OutOfRange`.

**Cause**

`VmArea::map_with_vmo` calls `create_mapping` before deciding whether to populate. Creation checks placement and permissions. The underlying VMO window is checked later by `VmObjectPaged::make_cursor`, which Lazy mode does not reach at creation.

**Change**

Validate the ordinary mapping window with the normalized size before publishing the mapping. Boot-time adoption of existing mappings needs its own internal entry because it currently uses a zero-length placeholder VMO.

**Reference**

Zircon `VmAddressRegion::CreateVmMapping` validates offset alignment and overflow, and checks physical VMO bounds before registration. The local user dispatcher already imposes VMO bounds for ordinary paged mappings.

**Acceptance**

An out-of-bounds ordinary mapping returns `OutOfRange` for Lazy, Resident and Commit and leaves its requested address available. Existing boot adoption still passes both dev boots.
