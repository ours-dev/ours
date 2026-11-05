# pmm-transfer: review of the PMM transfer path

**Date**: 2026-11-04

## Scope and method

Reviewed selection, zone ownership, zeroing, locality accounting and return validation. Ran the repository's clang-format and clang-tidy, the two dev images in both configurations, and the memory-only NUMA regression.

## What is done well

- `kernel/mm/include/ours/mm/pm_node.hpp:124` exposes the eligible range at the owner of the ordered zones.
- `kernel/mm/pm_node.cpp:213` counts the supplier against the captured initiator node.
- `kernel/commands/pm_node_test.cpp:564` observes the real per-CPU locality counter in the remote exhaustion case.

## High severity

None.

## Medium severity

Zone inventory and its atomic count still change in separate steps. The next design specifies one locked transfer so a concurrent withdrawal cannot subtract an unpublished return.

## Minor and style

None.

## Teaching opportunities

None.

## Suggested follow-up order

1. Commit each buddy mutation and its free-count delta in one zone-lock interval.
2. Define batch rollback and pressure observation at the operation boundary.
