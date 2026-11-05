# pmm-naming-review: delivered selected PMM names

**Date**: 2026-11-05

## Delivered

- `6271ea0f` names the zone free-frame counter getter num_free_frames and documents its snapshot contract.
- `06ac2bd5` replaces ZoneSelector with ZoneCandidates and zone_selector.hpp with zone_candidates.hpp.
- `c48667d6` replaces NodePlacement with CpuNodeMap, renames the source and header to cpu_node_map, and migrates the GN source entry.
- `59046bdc` nests Origin in AllocRequest and migrates the resolver and callers to AllocRequest::Origin.

- This follow-up names the threshold enum PmZone::Watermark, updates its four caller files and places each variant comment above its entry.

## Verification

Every implementation increment passed changed-line formatting and LLVM 23 clang-tidy for its affected C++ files. Both development images and all 163 host tests passed for each increment in the default configuration. The CpuNodeMap and Origin increments also passed both images and 163 host tests in the four-node NUMA configuration. Existing node and zone commands passed after the class renames, and node commands passed after nesting Origin. The host request smoke test retains its distinct node and CPU ids and multi-node mask checks.

The Watermark increment passed formatting and clang-tidy in all five C++ files. Default and four-node builds each booted both images and passed all 173 host tests. The default PMM command passed 38 checks, the zone KTEST passed its one case, the pressure command passed 19 checks and four-node PMM passed 77 checks.

## Deviations from the design

None.

## Found along the way

None.

## Not done

Other method-name candidates in REVIEW.md remain proposals pending the user's selection.
