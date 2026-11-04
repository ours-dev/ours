# pmm-inventory: review of zone inventory accounting

**Date**: 2026-11-04

## Scope and method

Reviewed both buddy transfers and the existing disabled cache branch. Verified the two-CPU, one-page inventory test, both dev images in both configurations, and the host suite.

## What is done well

- `kernel/mm/pm_zone.cpp:182` keeps the withdrawal and its count delta in one locked interval.
- `kernel/commands/pm_node_test.cpp:594` bounds the sample during concurrent transfers and compares both counts after the workers join.

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
