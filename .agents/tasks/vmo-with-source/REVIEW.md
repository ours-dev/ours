# vmo-with-source: review of the restored factory name

**Date**: 2026-11-04

## Scope and method

Verified that the restored kernel files match the tree before the factory rename. Ran the repository formatter, clang-tidy and default and NUMA boot checks. Ran the relevant pager and reclaim suites in both configurations.

## What is done well

- `kernel/mm/include/ours/mm/vm_object_paged.hpp:73` and `kernel/mm/include/ours/mm/vm_cow_pages.hpp:98` restore the same factory name at both layers.
- The naming guidance records Rust conventions as the default and Zircon as a secondary naming reference.

## High severity

None.

## Medium severity

None.

## Minor and style

None.

## Teaching opportunities

None requested.

## Suggested follow-up order

None.
