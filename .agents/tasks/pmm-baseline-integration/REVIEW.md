# pmm-baseline-integration: review of the test migration resolutions

**Date**: 2026-11-05

## Scope and method

Reviewed all eleven conflict resolutions against both parents and their change records. Used kernel-design, modern-cpp-development, cpp-write-and-verify and cpp-code-review. Checked runtime behavior with both development images, 173 host tests, default and four-node kernel tests, and allocation-failure tests. Ran LLVM 23 clang-tidy against both default and fault-enabled compile databases on the twelve resolved or adapted test sources; no diagnostics remained on those sources.

## What is done well

- `kernel/tests/vma_seal_test.cpp:336` retains the demand-fault rollback check and the retry after a page-table allocation failure.
- `kernel/tests/vma_undo_test.cpp:80` preserves the distinction between unmapping mappings and permitting complete subarea removal.
- `kernel/tests/dpc_test.cpp:229` preserves the baseline's intentional retention of state that a queued worker may still access on failure.
- The seven reconstructed MM suites retain all 176 original assertion sites while using the baseline's KTEST registrations. No deleted legacy command was reintroduced.

- The naming follow-up cleanly combines feature tip 5070f014 and baseline tip 8fb7e131. No conflict resolution changes test or allocator behavior. Both configurations passed both image boots and 173 host tests. Default preemption, hotplug, PMM and all registered tests passed; four-node PMM and zone checks passed. Incoming test changes passed formatting and clang-tidy on changed lines. Shared baseline edits have no path overlap.

## High severity

None.

## Medium severity

None in the conflict resolutions. The previously recorded preemption timing failure reappeared in the first instrumented full run; it is not diagnosed by this integration and is recorded in `docs/repairs/261026/kernel-time.md`.

## Minor and style

- The fault-enabled tests correctly report zero failed tests, but kshell_run.py treats expected page-table allocation error logs as an independent failure. Recorded in `docs/repairs/261105/kernel-test-framework.md`; the coverage runner confirms the actual test verdicts.
- Fixed one migration compilation error by passing a dynamic description as `"{}", entry.description` rather than using it as a compile-time format string.
- Kept the baseline's suite documentation and the branch's approved MM vocabulary. Searched kernel and system sources for retired identifiers; none remained.

## Teaching opportunities

None. This integration introduces no new architecture or synchronization decision.

## Suggested follow-up order

1. Give expected allocation failures an explicit test-scoped diagnostic mechanism without making unexpected errors pass.
2. Diagnose the preemption timing instability using the existing timer repair record.
