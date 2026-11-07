# scheduler-state-naming: Review of the per-thread state rename

**Date**: 2026-11-07

## Scope and method

Reviewed the canonical state definition and every renamed caller. Compared the new header against the previous definition after normalizing its type name and guard. Ran changed-line clang-format, clang-tidy, both QEMU images and the host suite for both RR and Fair.

## What is done well

The Fair type alias and private forwarding accessor refer to the same scheduling state used by the framework. Both Fair images booted. The compiler resolves the owning-thread lookup through the renamed field.

## High severity

None

## Medium severity

None

## Minor and style

None

## Teaching opportunities

### T1. A type alias preserves the object's identity

Fair's intrusive queue traits and the scheduler's owning-thread lookup use the same C++ type through either spelling. A wrapper type would create a separate type and would no longer carry that guarantee.

## Suggested follow-up order

None
