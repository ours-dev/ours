# pmm-naming: review of allocation vocabulary migration

**Date**: 2026-11-04

## Scope and method

Phase 1 reviews the AllocFlags/header, preset, parameter/member, and request-accessor rename across active kernel, heap, VMO, object, benchmark, and host-test consumers. Read kernel-design, modern-cpp-development, cpp-write-and-verify, and local format/tidy configuration. Flags retain their original values; CanBlock is the former unused DirectReclaim hint. Phase 2 also covers AllocPolicy/AllocState, their headers, and thread accessors. The helper slice remains separate.

Ran changed-line clang-format, GN formatting, and real LLVM 23 clang-tidy on all 109 touched C++ files with the actual out/vm-address flags. Captured pre-change diagnostics for 108 files and normalized renamed identifiers/paths when comparing: zero new diagnostics. The additional benchmark caller has no diagnostics on its edited line. The boot-options header was checked with its real kernel/top/boot_options.cpp flags because automatic header inference chose an incomplete include context.

Built the complete default graph, then booted both disked dev images with a QEMU snapshot overlay: dbg 46 passed / 5 skipped, rel 44 passed / 8 skipped and 7/7 required. All 163 host test executables passed. Logs: /tmp/pmm-naming-checks/flags-build.log, flags-verify.log, and flags-before/flags-after.

Phase 2: all nine touched C++ files passed formatting and clang-tidy with zero new diagnostics against their own before snapshot. The default graph built in 24.7 seconds; disked dbg/rel checks and all 163 host test executables passed with the same shell counts. The policy switch, disable counter, object layout, and locking are unchanged. Logs are /tmp/pmm-naming-checks/state-verify.log and state-before/state-after.

## What is done well

- alloc_flags.hpp preserves enum bit positions and named preset combinations. Its current-behavior comments distinguish blocking capability, caller-managed waiting, zeroing, and conditional ThisNode narrowing.
- AllocRequest::flags() and all callers migrate together. Old live-source Gaf/GAF/gaf spellings and header includes are absent.
- Main Pm/Vm model names, locking, selection, fallback policy, reclaim, and strong address domains retain their behavior. No compatibility alias or new policy mechanism was added.

## High severity

None found in phases 1 and 2.

## Medium severity

None introduced by phases 1 and 2. Existing watermark admission, policy enablement, and shared-policy scaffolding remain separate architectural work.

## Minor and style

- Pre-existing tidy diagnostics remain; no repository-wide suppression was introduced.
- Node-specific get_frame_n still references a nonexistent helper at this phase boundary; the next dedicated fix/coverage slice addresses it.

## Teaching opportunities

None requested.

## Suggested follow-up order

1. Unify alloc_frames_n/get_frames_n and exercise both direct-map count-helper overloads through the production PMM route.
2. Record final validation and keep the larger responsibility design explicitly Proposed.
