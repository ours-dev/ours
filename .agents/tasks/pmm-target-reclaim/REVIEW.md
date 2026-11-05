# pmm-target-reclaim: review of node-directed background reclaim

**Date**: 2026-11-04

## Scope and method

Reviewed the order-zero allocation miss, deferred zone sampling, and the node-scoped reclaim walk. Ran clang-format, clang-tidy, the host suite, the default and NUMA dev images, and the memory-only node reclaim test.

## What is done well

- `kernel/mm/pm_node.cpp` records a shortage only from the eligible zone walk and clears it when allocation succeeds.
- `kernel/mm/memory_reclaimer.cpp` coalesces requests by zone and resamples them before deriving the node target.
- `kernel/commands/page_reclaim_test.cpp` verifies a failed strict allocation, reclaim on the exhausted node, successful retry and preservation of the other node's page.

## High severity

None.

## Medium severity

None.

## Minor and style

The worker samples advisory deficits and can reclaim any eligible page in the target node. The global admission gate continues to govern `ShouldWait`.

## Teaching opportunities

None.

## Suggested follow-up order

None.
