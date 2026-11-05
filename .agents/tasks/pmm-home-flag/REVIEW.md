# pmm-home-flag: review of the conditional allocation flag rename

**Date**: 2026-11-04

## Scope and method

Reviewed the flag declaration, resolver, and kernel and host test callers.
Checked the diff against the documented mask policy and ran clang-format,
clang-tidy, the host tests, and QEMU boot and PMM checks.

## What is done well

- `kernel/mm/include/ours/mm/alloc_flags.hpp:25` describes what the flag does when the mask includes or excludes the home node.
- `kernel/mm/tests/alloc_request_smoke.cpp:87` retains the case where the home node is excluded.

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
