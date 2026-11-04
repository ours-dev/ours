# shared-vmo-views: review of COW notification and representative handoff

**Date**: 2026-11-04

## Scope and method

Reviewed view membership, intrusive hook lifetime, lock ownership and page
mutation ordering against the kernel design and the Zircon counterparts.
Clang-format and clang-tidy checked all five C++ files. Both default and
NUMA dev images booted. All host tests and targeted kernel suites passed.

## What is done well

- `kernel/mm/vm_object_paged.cpp:44` removes or promotes the representative
  under the tree lock and drops the COW reference after that lock is released.
- `kernel/mm/vm_object_paged.cpp:70` sends each view the original range,
  including ranges outside the representative's own window.
- `kernel/commands/vmo_snapshot_test.cpp:727` exercises successive
  handoffs and observes their effects through page-table entries.

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

## Follow-up on local range clipping

The local lambda runs synchronously under the shared tree lock for the
representative and each peer. Saturating the end once preserves the previous
overflow behavior. Each view still intersects the original range and
translates the result using its own offset. Clang-tidy found no issues on
the changed lines. The snapshot suite passed 247 checks in both
configurations, including the case where only a peer overlaps the range.

## Follow-up on notification offset types

The notification path now carries object-relative byte offsets as `usize`.
The slice constructor converts its checked offset after establishing that
the slice fits inside the parent window. Both intersections cap the range
end before addition. The snapshot suite covers whole-range invalidation
and handoff to a view with a disjoint window. The default and NUMA runs
passed, and clang-tidy reported no diagnostics on changed lines.
