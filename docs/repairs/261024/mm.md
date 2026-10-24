# mm outstanding repairs

## 1. A map of a physical mapping maps every region after the first from the wrong address

**Severity**
Medium: wrong frames mapped, but only for a range with more than one protection region, and `VmMapping::map()` is called only when a mapping is made, when its range is one region.

**Symptom**
Latent, cannot be triggered today. `VmMapping::map_physical()` over a range whose protection changes part way (after a `protect()` of part of the mapping) maps the second region's pages to the physical addresses of the first region's.

**Cause**
`map_physical()` looks up `phys_base`, the physical address of `base`, the start of the whole range, then for each protection region appends `phys_base + offset` with `offset` counted from the region's own start. For the first region the two starts are the same; for any later one the region's distance from `base` is missing.

**Change**
Append `phys_base + (region_base - base) + offset`, as `map_paged()` effectively does by stepping its cursor through the whole range.

**Reference**
Zircon `VmMapping::MapRange()`, which calls `LookupContiguousLocked()` for each protection range with that range's own object offset.

**Acceptance**
A kshell check that maps a physical object without Populate, protects its second page read-only, calls `map()` over both pages, and finds each entry on its own frame.
