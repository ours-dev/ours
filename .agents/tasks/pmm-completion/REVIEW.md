# pmm-completion: review of pressure observation

**Date**: 2026-11-04

## Scope and method

Reviewed admission, shared home resolution, rollback ordering, and the completed-operation pressure sample. Ran clang-format, clang-tidy, both dev images, and the direct-operation pressure tests.

## What is done well

- `kernel/mm/pm_node.cpp` declares batch rollback after the observation guard, so rollback runs before observation on error.
- `kernel/commands/memory_pressure_test.cpp` tests both threshold crossings and a failed batch whose temporary allocation crosses a threshold.

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
