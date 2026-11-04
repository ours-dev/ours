# pmm-naming: converge physical allocation names without changing allocation policy

**Date**: 2026-11-04
**Status**: Accepted

## Request

The user asked to refactor PMM after reviewing its responsibilities, then emphasized convergence with the existing naming vocabulary. This slice implements the naming work: allocation flags, policy/state, and the count helpers. The larger object decomposition remains a separate proposed design. Main Pm/Vm models retain their current names.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope of this implementation | Naming slice; entire object decomposition | Land the naming slice first, preserving behavior | Not requested | Naming refactor requested, 2026-11-04 |
| Flag semantics | Rename and preserve values; change admission | Preserve values and behavior | Not requested | Naming slice only |

## Steps

1. Rename Gaf/header, allocation presets, parameters, members, and request accessor to the AllocFlags vocabulary. Rename the unused DirectReclaim hint to CanBlock without adding a consumer or changing its bit. Format/tidy every touched C++ file, build/boot both disked dev images, and run all host tests before the naming commit.
2. Rename MemoryAllocationPolicy/State, headers, and thread accessors to AllocPolicy/State. Preserve enablement, policy behavior, and locking. Repeat required source and build/boot checks, then commit independently.
3. Unify the minimum-count helper names as alloc_frames_n/get_frames_n, fix the node-specific helper's nonexistent call, and cover the real public helper through the existing pmnode kernel test. Verify formatting/tidy, both images, host tests, and relevant PMM/VMO/pressure kernel suites, then commit.
4. Record delivered commits and verification in RESULT.md. Keep unimplemented architectural proposals explicitly Proposed.

## Files

- kernel/mm/include/ours/mm/alloc_flags.hpp replaces gaf.hpp, with all active source/build consumers migrated.
- kernel/mm/include/ours/mm/alloc_state.hpp and priv/alloc_policy.hpp replace their long-named headers; thread and PMM call sites migrate together.
- kernel/mm/include/ours/mm/pmm.hpp and kernel/commands/pm_node_test.cpp contain the public helper fix and coverage.
- .agents/tasks/pmm-naming holds this plan, self-review, and result; the earlier design documents retain their separate status.

## Test plan

Use prebuilt LLVM 23 clang-format on changed lines and clang-tidy with out/vm-address for every touched C++ file. Compare pre-existing diagnostics before/after the mechanical rename. Use the repository boot checker with DISK=/tmp/address-integration-ghfjh50g/test-gpt.img, which QEMU opens with snapshot=on, and require both image verdicts and all host test executables to pass. Run pmnode and relevant contiguous VMO, pin, pager/reclaim, pressure and address suites on the final dbg image. Reproduce the formerly broken template with the actual kernel compile flags.
