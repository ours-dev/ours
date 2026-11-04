# vmo-create-external: review of the external page source factory rename

**Date**: 2026-11-04

## Scope and method

Compared the two factories with Zircon's `VmObjectPaged::CreateExternal` and `VmCowPages::CreateExternal`. Verified that the C++ diff contains only the renamed identifier and whitespace. Ran the repository formatter and clang-tidy on all nine files, booted both dev images in the default and four-node NUMA configurations and ran the external page source kernel suites.

## What is done well

- `kernel/mm/vm_object_paged.cpp:121` and `kernel/mm/vm_cow_pages.cpp:440` use one name for creation with an external page source.
- `kernel/object/pager_dispatcher.cpp:89` uses the renamed factory on the pager creation path.

## High severity

None.

## Medium severity

None.

## Minor and style

No clang-tidy diagnostics on the changed lines.

## Teaching opportunities

None requested.

## Suggested follow-up order

None.
