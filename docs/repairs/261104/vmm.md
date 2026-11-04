# vmm outstanding repairs

## 1. A lazy kernel mapping can describe a window beyond its VMO

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
