# pmm-unified-allocation: review of allocation admission

**Date**: 2026-11-04

## Scope and method

Reviewed the unified block entry, typed adapters, and callers that either handle `ShouldWait` or map allocation errors to their existing contracts. Checked the optional address output, gate timing, and the wakeup handoff. Ran clang-format, clang-tidy, host tests, and the default and NUMA QEMU runs.

## What is done well

- `kernel/mm/pmm.cpp:390` shares admission across block overloads and passes an immediate request to `PmNode`.
- `kernel/commands/memory_pressure_test.cpp:180` verifies `ShouldWait` for the explicit-node and address-output forms without writing an address on failure.
- `kernel/mm/include/ours/mm/pmm.hpp:277` records the default-placement, single-page wait contract.

## High severity

None.

## Medium severity

None.

## Minor and style

None.

## Teaching opportunities

None.

## Suggested follow-up order

None.
