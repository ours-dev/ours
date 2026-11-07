# scheduler-switch: Review of context switch completion

**Date**: 2026-11-07

## Scope and method

Reviewed the first-run and resumed switch paths against the kernel design and C++ conventions. Ran changed-line clang-format, clang-tidy, a Fair syntax check and both QEMU boots.

## What is done well

The completion helper still consumes the resuming CPU's saved previous thread and releases its PI lock. The first-run path still releases its own PI lock before enabling interrupts. ThreadJoin, WaitQueue and Hotplug passed on four CPUs.

## High severity

None

## Medium severity

None

## Minor and style

None

## Teaching opportunities

### T1. Release the lock belonging to the CPU that resumed the thread

A parked thread can wake on a different CPU. Its old stack frame can still contain the original scheduler pointer. `Current::get()` resolves the CPU where the thread is now executing and gives completion the correct previous thread.

## Suggested follow-up order

None
