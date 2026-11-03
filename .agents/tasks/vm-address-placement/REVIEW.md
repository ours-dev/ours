# vm-address-placement: review of strong address migration

**Date**: 2026-11-03

## Scope and method

Self-review of the VirtAddr/PhysAddr migration before removing VmPlacement.
Read modern-cpp-development, kernel-design, the affected address and paging
interfaces, and the local Zircon vm_address_region.cc creation paths.
Ran repository clang-format and clang-tidy on all 138 changed C++ files;
three headers were additionally checked through actual including translation
units because standalone inference lacked their include context. New naming
and override diagnostics were corrected. Existing unrelated diagnostics remain.
Both development images and 163 host tests passed. Fourteen relevant kernel
suites passed, including usercopy, physical mappings, snapshots, large pages,
BTI, resources, page sources, pager and reclamation.

## What is done well

- system/public/ours/types.hpp:14 preserves distinct scoped address types;
  offsets, address differences and machine encodings retain their own types.
- kernel/mm/tests/address_smoke.cpp:23 rejects cross-domain operations and
  implicit integer conversions at compile time, and distinguishes physical
  zero from failed physmap translation.

## High severity

None outstanding in this increment.

## Medium severity

None outstanding in this increment.

## Minor and style

Resolved new constexpr naming and redundant virtual diagnostics. Three header
checks required a real including translation unit instead of inferred flags.

## Teaching opportunities

None; the user asked to focus on development.

## Suggested follow-up order

1. Remove VmPlacement and reject unknown reserved virtual values before alignment.
2. Exercise zero, fixed placement, Anywhere, overlaps and reserved-range edges.
