# pmm-thisnode: review of the strict NUMA allocation flag

**Date**: 2026-11-04

## Scope and method

Reviewed the mask intersection, the thread policy bypass, and the common
single, bulk and fragment allocation entry points. Compared the placement
contract to Linux's no-fallback zonelist and explicit nodemask filtering.
Ran clang-format, clang-tidy, host and QEMU tests.

## What is done well

- `kernel/mm/include/ours/mm/priv/alloc_request.hpp:219` applies the mask intersection even when it is empty.
- `kernel/commands/pm_node_test.cpp:578` compares strict failure with ordinary fallback on the same exhausted node.

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
