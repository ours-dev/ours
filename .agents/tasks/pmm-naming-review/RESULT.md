# pmm-naming-review: delivered selected PMM names

**Date**: 2026-11-05

## Delivered

- `6271ea0f` names the zone free-frame counter getter num_free_frames and documents its snapshot contract.
- `06ac2bd5` replaces ZoneSelector with ZoneCandidates and zone_selector.hpp with zone_candidates.hpp.
- `c48667d6` replaces NodePlacement with CpuNodeMap, renames the source and header to cpu_node_map, and migrates the GN source entry.
- The commit containing this result nests Origin in AllocRequest and migrates the resolver and callers to AllocRequest::Origin.

## Verification

Every implementation increment passed changed-line formatting and LLVM 23 clang-tidy for its affected C++ files. Both development images and all 163 host tests passed for each increment in the default configuration. The CpuNodeMap and Origin increments also passed both images and 163 host tests in the four-node NUMA configuration. Existing node and zone commands passed after the class renames, and node commands passed after nesting Origin. The host request smoke test retains its distinct node and CPU ids and multi-node mask checks.

## Deviations from the design

None.

## Found along the way

None.

## Not done

Other method-name candidates in REVIEW.md remain proposals pending the user's selection.
