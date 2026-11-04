# pmm-naming: review of allocation vocabulary migration

**Date**: 2026-11-04

## Scope and method

Phase 1 reviews the AllocFlags/header, preset, parameter/member, and request-accessor rename across active kernel, heap, VMO, object, benchmark, and host-test consumers. Read kernel-design, modern-cpp-development, cpp-write-and-verify, and local format/tidy configuration. Flags retain their original values; CanBlock is the former unused DirectReclaim hint. Phase 2 also covers AllocPolicy/AllocState, their headers, and thread accessors. Phase 3 unifies the public minimum-count helpers and covers the direct-map overloads in pmnode.test.

Ran changed-line clang-format, GN formatting, and real LLVM 23 clang-tidy on all 109 touched C++ files with the actual out/vm-address flags. Captured pre-change diagnostics for 108 files and normalized renamed identifiers/paths when comparing: zero new diagnostics. The additional benchmark caller has no diagnostics on its edited line. The boot-options header was checked with its real kernel/top/boot_options.cpp flags because automatic header inference chose an incomplete include context.

Built the complete default graph, then booted both disked dev images with a QEMU snapshot overlay: dbg 46 passed / 5 skipped, rel 44 passed / 8 skipped and 7/7 required. All 163 host test executables passed. Logs: /tmp/pmm-naming-checks/flags-build.log, flags-verify.log, and flags-before/flags-after.

Phase 2: all nine touched C++ files passed formatting and clang-tidy with zero new diagnostics against their own before snapshot. The default graph built in 24.7 seconds; disked dbg/rel checks and all 163 host test executables passed with the same shell counts. The policy switch, disable counter, object layout, and locking are unchanged. Logs are /tmp/pmm-naming-checks/state-verify.log and state-before/state-after.

Phase 3: formatting and clang-tidy passed on both touched files with no new diagnostics. A compile probe using the real PMM flags failed before the node-specific helper fix with undeclared get_frame, then compiled after the fix. Both disked development images and all 163 host tests passed again. Ten kernel suites passed: pmnode (23 checks, including both new direct-map cases), pmzone, vmocontig, vmopin, vmosnapshot, pager, pagereclaim, mempressure, vmaddress, and bti. Logs: helper-verify.log, public_api_before/after.log, and kernel-tests.log under /tmp/pmm-naming-checks.

The extra NUMA build exposed an existing strong-address boundary omission in kernel/arch/x86/mm/srat.cpp: a raw u64 is passed to EarlyMem::set_node(PhysAddr, ...). The naming changes do not touch that line. That boundary fix was validated separately: clang-tidy under the actual NUMA flags has no new diagnostics, and both four-node images plus all 163 host tests pass. The four-node PMM, zone, contiguous-VMO, and pressure kernel suites pass (59/9/16/15 checks).

The additional NUMA33 check is not a pass: both images finish booting but fail the first vmalloc single-page self-check. LLDB confirmed map_bulk returns NoMemory before any page is mapped. A pre-refactor f7b22de4 source snapshot with only the same SRAT cast reproduces the identical failure, confirming it is not introduced by this naming slice. The unresolved cause is recorded in docs/repairs/261104/mm.md; no watermark, mapping, or self-test behavior was changed to hide it.

## What is done well

- alloc_flags.hpp preserves enum bit positions and named preset combinations. Its current-behavior comments distinguish blocking capability, caller-managed waiting, zeroing, and conditional ThisNode narrowing.
- AllocRequest::flags() and all callers migrate together. Old live-source Gaf/GAF/gaf spellings and header includes are absent.
- Main Pm/Vm model names, locking, selection, fallback policy, reclaim, and strong address domains retain their behavior. No compatibility alias or new policy mechanism was added.

## High severity

None introduced by the naming/helper changes.

## Medium severity

None introduced by the naming/helper changes. Existing watermark admission, policy enablement, and shared-policy scaffolding remain separate architectural work.

## Minor and style

- Pre-existing tidy diagnostics remain; no repository-wide suppression was introduced.
- The node-specific helper typo is fixed and instantiated by a production-path kernel test. Both helper families now use the plural frames_n spelling; power-of-two rounding is retained.

## Teaching opportunities

None requested.

## Suggested follow-up order

1. The naming changes and SRAT boundary repair are separate commits; see RESULT.md for hashes and evidence.
2. Follow docs/repairs/261104/mm.md for the independent NUMA33 vmalloc failure.
3. Keep the larger responsibility decomposition explicitly Proposed.
