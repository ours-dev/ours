# pmm-static: PmNode allocation and release operate statically across nodes

**Date**: 2026-11-04

## Delivered

Source commit `2a6d6a00` on codex/pmm-naming implements the user's selected interface:

```cpp
PmNode::alloc_frames(alloc_flags, order, allowed_nodes);
PmNode::alloc_frames(home, alloc_flags, order, allowed_nodes);
PmNode::free_frames(frame, order);
PmNode::free_frames(&frames);
```

Allocation returns Result<PmFrame *>. The implicit form resolves the local/default-policy home; the explicit form starts from the named node, with the existing out-of-range hint convention selecting the implicit form. Both may use the permitted fallback set, with the existing ThisNode rule. Bulk and fragmented allocation have matching static forms. Release was already static and continues to route through each frame's own node and zone.

All active PmNode allocation callers use class qualification. pmm.cpp retains the pressure gate and pointer/Status adapters, while default-home policy resolution moved beside static node allocation. Private instance helpers still use the selected node's resource state. No standalone FrameAllocator class, new per-request allocation, node-layout change, or global allocation lock is introduced.

The responsibility design and kernel-design reference now reflect the user's static-interface decision. Other resource/registry/CPU-decomposition proposals remain independent.

## Verification

Real LLVM 23 changed-line formatting and clang-tidy ran on all four edited C++ files with out/vm-address flags. Before/after diagnostics contain no new diagnostics. git diff --check and changed-document lint pass.

| Configuration | dbg | rel | Host executables |
|---|---|---|---|
| Default, disked | 46 passed, 5 skipped, no errors | 44 passed, 8 skipped, 7/7 required, no errors | 163 passed |
| Four-node NUMA, disked | 46 passed, 5 skipped, no errors | 44 passed, 8 skipped, 7/7 required, no errors | 163 passed |

Default kernel suites pass: pmnode 28, pmzone 3, vmocontig 16, mempressure 15, pager 23, pagereclaim 21 checks. Four-node suites pass: pmnode 65, pmzone 9, vmocontig 16, mempressure 15 checks.

The new cases exercise class-qualified allocation without an instance, explicit home/ThisNode, an empty allowed set, oversized order rejection, permitted remote allocation, and direct implicit/explicit allocation-disable enforcement. Successful allocations, including unexpected-success error paths, are returned through static release.

Logs/manifests are in /tmp/pmm-static-checks/: static-before/static-after, new-diagnostics.json, format.log, default-verify.log, numa-verify.log, default-kernel-tests.log, and numa-kernel-tests.log. Disk runs use QEMU snapshot overlays.

## Deviations from the design

None for the user's selected static-interface slice. FreeMemoryGate and one-page waiting remain in the existing pmm facade. Other responsibility proposals were not bundled into this change.

## Found along the way

None new. The previously reproduced NUMA33 vmalloc startup issue remains separately tracked in docs/repairs/261104/mm.md; this slice does not claim a NUMA33 pass or repeat that unrelated investigation.

## Not done

Registry extraction, CPU-relationship consolidation, actual policy implementation/enablement, selector watermark admission, zone ordering, and the per-CPU cache remain separate work. Existing ownership preconditions and locality-counter semantics are retained.
