# pmm-naming-review: review of PMM interface vocabulary

**Date**: 2026-11-05

## Scope and method

Reviewed pmm.hpp, pm_node.hpp, pm_zone.hpp, alloc_state.hpp, priv/frame_buddy.hpp and priv/alloc_request.hpp, together with pm_node.cpp, pm_zone.cpp and frame_buddy.cpp. The scope is the user's requested naming list. Read kernel-design and modern-cpp-development and traced the candidate operations through their definitions and existing callers. The proposals below are source-based judgments. LLVM 23 clang-format dry-run checks passed for all nine files. clang-tidy completed for all nine with existing style diagnostics and no compile errors. The existing built debug and release images passed, together with all 163 host tests. No PMM implementation file was modified. Tool logs are in /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-pmm-names-review-9ntbob0i.

The local references are Linux mm/page_alloc.c, including build_zonelists, its free_area::nr_free accounting and the allocation families declared in include/linux/gfp.h. The Zircon references are kernel/vm/include/vm/pmm.h and kernel/vm/pmm.cc, including pmm_alloc_pages, pmm_alloc_contiguous and pmm_count_free_pages. The implementation retains the project's established node and zone resource models.

## What is done well

- PmFrame, PmZone and PmNode remain the principal resource names. PmNode::alloc_frames and free_frames are the user-selected static operations.
- Gaf retains the name explicitly selected by the user. AllocState, AllocPolicy, AllocRequest, ZoneList, ZoneSelector and FrameBuddy have established local roles.
- take_block and return_block name the physical transfer at the zone boundary. candidates describes the filtered zone order.
- The allocation and transfer paths distinguish blocks, exact batches of order-zero frames and power-of-two rounding. The proposed names should preserve those distinctions.

- The selected num_free_frames getter and its callers passed changed-line formatting and clang-tidy in all six C++ files. Both development images and 163 host tests passed, together with the existing node, zone and pressure commands.

- ZoneCandidates is implemented with the renamed header and migrated callers. Both development images, 163 host tests and the node and zone commands passed. All four C++ files passed changed-line formatting and clang-tidy.

- CpuNodeMap is implemented with matching source and header names. Default and four-node NUMA builds booted both images, passed 163 host tests each and passed node and zone commands. All nine C++ files passed formatting and clang-tidy.

- AllocRequest::Origin preserves the record fields and resolver logic. All four C++ files passed formatting and clang-tidy. Default and four-node NUMA configurations booted both images, passed 163 host tests each and passed pmnode.test.

- PmZone::Watermark preserves the unsigned base, enumerator order and array extent. All five edited C++ files are token-identical to their parents after normalizing the selected name and comments. Formatting and clang-tidy passed. Default and four-node builds booted both images and passed 173 host tests each. The default PMM, zone and pressure tests passed; the four-node PMM test passed 77 checks.

## High severity

None established within this naming review.

## Medium severity

None established within this naming review.

## Minor and style

The table records the initial proposals. The user subsequently selected num_free_frames for item 2, superseding its proposed approximate-count spelling. Items 2, 9 and 10 are implemented. The user also accepted item 6, scheduled as the next naming increment. Items 1, 3, 4, 5, 7 and 8 remain proposals.

| Item | Current name | Proposed name | Evidence and reason |
|---|---|---|---|
| 1 | PmZone::contains, PmNode::contains | has_free_block | pm_zone.hpp:123 and pm_node.hpp:331. The argument is an order. The implementation scans free lists at that order and above for a splittable block. This is an availability snapshot rather than an address-membership test. |
| 2 | PmZone::free_frames | approx_free_frame_count | pm_zone.hpp:174. This reads an approximate counter. The current spelling also names the operation that returns allocated frames in the public and node interfaces. |
| 3 | PmZone::count_free_frames | free_frame_count | pm_zone.hpp:140. This locks and counts frames in buddy lists. It matches the existing public free_frame_count and pairs with the approximate query. |
| 4 | FrameBuddy::free_count, PmZone::free_blocks | free_block_count | priv/frame_buddy.hpp:67 and pm_zone.hpp:146. The count is free blocks of one exact order. pm_zone.cpp shifts the buddy count by the order when it needs a frame count. |
| 5 | alloc_frames_n, get_frames_n | alloc_frames_at_least, get_frames_at_least | pmm.hpp:136, 153, 174 and 195. The argument is a minimum frame count. Three frames round to a four-frame block in pm_node_test.cpp. The name must not imply exact-count allocation. |
| 6 | AllocState::get_policy | policy | alloc_state.hpp:37. This is a getter for the held allocation policy. The short noun follows the existing flags, order and kind accessors. |
| 7 | PmZone::which_node | nid | pm_zone.hpp:151. This returns the zone's node id. nid is already used by PmNode and PageFrame, so the proposal converges the existing spelling. |
| 8 | ZoneList::take_local_zones | add_local_zones | pm_node.hpp:64. This appends borrowed pointers to the fallback order. It transfers no zone ownership or physical frames. |
| 9 | PmZone::WaterMark | Watermark | pm_zone.hpp:58. Use the single domain word already spelled watermark in the query methods. |
| 10 | Origin | AllocRequest::Origin | priv/alloc_request.hpp:34. The node and initiating CPU record belongs to resolved allocation requests. Nesting it supplies that context at callers without a new role suffix. |

Items 1 through 5 are the highest-value proposals because they expose behavior or the counted unit. Items 6 through 10 primarily converge spelling and type context. Exact batch allocation remains a distinct operation from the rounded block helpers in item 5.

The selected getter reads the atomic counter without acquiring the zone lock. Counter updates use the actual frame deltas, and the comment distinguishes a snapshot during an in-flight transfer from the locked buddy-list traversal.

### Class names selected by the user

| Item | Current name | Proposed name | Evidence and reason |
|---|---|---|---|
| C1 | ZoneSelector | ZoneCandidates | priv/zone_selector.hpp:33 and pm_node.hpp:125. The object is a repeatable filtered range returned by ZoneList::candidates. It holds a zone order and a request, and exposes begin/end. PmNode's allocation path decides which candidate ultimately supplies the block. |
| C2 | NodePlacement | CpuNodeMap | node_placement.hpp:54. The owned state is NODE_OF_CPU and its per-CPU node pointer cache. node_of and set_node_of maintain the runtime CPU-to-node relation. Allocation policy and fallback selection are separate responsibilities. |

The user accepted C1, C2 and AllocRequest::Origin on 2026-11-05. C1, C2 and AllocRequest::Origin are implemented. The strongest class rename is C1 because the existing candidates factory already states the returned object's role. C2 removes ambiguity about what is placed and where the information lives. The previously proposed AllocRequest::Origin remains a separate type-scoping candidate.

PmFrame, PmZone, PmNode, FrameBuddy, AllocRequest, AllocPolicy, AllocState, ZoneList, MemoryReclaimer and MemoryWatchdog already express their current roles. MemoryPressureLevels contains thresholds, debounce and hysteresis, so a shorter thresholds-only name would need care about the complete contract.

The additional ZoneSelector and NodePlacement header/source checks completed with LLVM 23 clang-format dry-run and clang-tidy, all with exit code zero. clang-tidy emitted existing diagnostics. Logs are in /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-pmm-class-names-17yyd5rd. This extension changes review text only and reuses the latest verified implementation at 6271ea0f.

## Teaching opportunities

None requested.

## Suggested follow-up order

1. Let the user select or revise entries in the numbered table.
2. Implement approved entries as small naming changes with migrated callers and the repository's normal build, boot and source checks.
