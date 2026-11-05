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

## High severity

None established within this naming review.

## Medium severity

None established within this naming review.

## Minor and style

All entries are proposals. No entry is an approved implementation change.

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

## Teaching opportunities

None requested.

## Suggested follow-up order

1. Let the user select or revise entries in the numbered table.
2. Implement approved entries as small naming changes with migrated callers and the repository's normal build, boot and source checks.
