# pmm-gaf: review of the frame-allocation flag rename

**Date**: 2026-11-04

## Scope and method

Reviewed the declaration and type references across PMM, VM, heap, KTL and
tests. Compared the code diff to the direct identifier substitution and
checked the additional formatting and using-declaration ordering. Ran
clang-format, clang-tidy, the host tests and QEMU boot and NUMA checks.

## What is done well

- `kernel/mm/include/ours/mm/alloc_flags.hpp:13` defines the flag type once.
- `kernel/lib/ktl/include/ktl/types.hpp:16` exposes the same type to KTL.

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
