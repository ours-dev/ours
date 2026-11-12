# task-scheduler-layout: review of the scheduler move

**Date**: 2026-11-12

## Scope and method

Reviewed the scheduler move, the GN source set, downstream include and namespace updates, and the LLDB helper against the kernel design and C++ conventions. Ran the RR and Fair boot and host suites, targeted kernel tests, and changed-line format and clang-tidy checks. Compared the Fair source and header after normalizing the namespace, header guard, include paths, and type names.

## What is done well

- `kernel/kernel/task/BUILD.gn:42` keeps the source selection and dependencies in a separate scheduler target under task.
- `scripts/lldb_ours.py:409` and `scripts/lldb_ours.py:669` resolve the names now present in the debug image.

## High severity

None

## Medium severity

None

## Minor and style

- Clang-tidy continues to report the existing naming diagnostic for the Fair member `current_`.

## Teaching opportunities

`kernel/kernel/task/BUILD.gn:42` shows how a source set can keep its dependency list and build-time discipline selection after its implementation files move into a shared directory.

## Suggested follow-up order

None
