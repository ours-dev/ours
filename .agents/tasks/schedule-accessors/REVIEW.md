# schedule-accessors: Review of named scheduler dispatch

**Date**: 2026-11-07

## Scope and method

Reviewed the Ref accessor and every migrated Schedule call against the C++ and scheduler conventions. Ran changed-line clang-format, clang-tidy, paired compiler probes and QEMU tests.

## What is done well

The named accessor checks the supplied queue's capability. The compiler rejected an enqueue call holding another queue's lock. The host test verifies that a saved accessor owns its borrowed handle even after the original Ref temporary expires.

The follow-up moved both lookups to internal linkage and moved the two handover definitions without changing their bodies or lock declarations. Symbol checks on both built scheduler objects found no externally linked lookup and found both handover symbols for the PI caller. Both QEMU images and the host suite passed.

## High severity

None

## Medium severity

None

## Minor and style

None

## Teaching opportunities

### T1. Check the lock before dispatch erases the call

The generated member is a normal function as far as Clang is concerned. Its declaration can name the run queue argument and require its lock before the member calls the function pointer from the table.

## Suggested follow-up order

None
