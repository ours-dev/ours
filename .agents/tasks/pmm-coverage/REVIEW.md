# pmm-coverage: review of PMM regression tests

**Date**: 2026-11-04

## Scope and method

Reviewed the PMM kernel and host test diff using the kernel design and C++
conventions. Ran clang-format, clang-tidy, both QEMU dev images, the
memory-only NUMA test and host coverage.

## What is done well

- `kernel/commands/pm_node_test.cpp:552` checks rollback by frame identity and releases test allocations before reporting.
- `kernel/commands/memory_pressure_test.cpp:246` verifies that rejected registrations preserve the armed event.
- `kernel/mm/tests/frame_buddy_smoke.cpp:117` exercises the node and zone guards in an isolated allocator.

## High severity

None.

## Medium severity

None.

## Minor and style

The exhaustion test assumes an idle guest with a memory-only node and requires explicit invocation.

## Teaching opportunities

None.

## Suggested follow-up order

None.
