# pmm-static: make PmNode allocation and release static operations across nodes

**Date**: 2026-11-04
**Status**: Accepted

## Request

The user chose PmFrame/PmZone/PmNode as the principal resource models and requested PmNode::alloc_frames and free_frames as static operations across all nodes. Keep per-node resource state in instances. This supersedes the proposed separate FrameAllocator execution class.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Allocation/release owner | Separate allocator class; PmNode static interface | Static PmNode operations | Not requested | User requested, 2026-11-04 |
| Entry shapes | Only explicit home node; implicit and explicit home | Keep the existing shape and add an explicit-node overload; both are static | Not requested | Preserve local-first and allowed-node fallback |
| Pressure/wait protocol | Move it too; retain the existing pmm facade | Retain the current gate and single-page wait protocol | Not requested | No pressure-policy change in this slice |

## Steps

1. Add implicit-home and explicit-home static allocation overloads. Convert bulk/fragment allocation coherently, keep free_frames static, and preserve validation and node-owned zone state.
2. Route public pmm adapters through the static interface. Move implicit-home policy resolution to the node allocation implementation so direct static callers use the same placement rules. Explicit nodes keep precedence over thread-default policy.
3. Exercise direct class calls, all-node eligibility/fallback, static release, error paths, and direct allocation-disable enforcement in pmnode.test.
4. Run changed-line format, actual clang-tidy, default and four-node NUMA build/boot/host checks, and related kernel suites. Self-review and commit this complete interface slice.

## Files

- kernel/mm/include/ours/mm/pm_node.hpp: static allocation contracts and overloads.
- kernel/mm/pm_node.cpp: selection of the home-node resource object, static allocation loops, and implicit-home resolution.
- kernel/mm/pmm.cpp: static forwarding with unchanged pressure/wait handling.
- kernel/commands/pm_node_test.cpp: actual static calls and cross-node behavior.
- Kernel design reference and pmm-responsibilities design: record the user's selected execution interface.

## Test plan

Compare before/after clang-tidy diagnostics using out/vm-address and check changed-line formatting on all four C++ files. Build/boot both default disked images and both four-node NUMA images, with all host tests. Run pmnode, pmzone, vmocontig, and mempressure kernel commands in both configurations. The known pre-existing NUMA33 vmalloc failure in docs/repairs/261104/mm.md is not changed by this interface refactor.
