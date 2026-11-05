# pmm-design-review: review of PMM allocation contracts and layering

**Date**: 2026-11-03

## Scope and method

Follow-up on 2026-11-04: the naming/helper findings were addressed by the commits recorded in `.agents/tasks/pmm-naming/RESULT.md`. This report retains its pre-refactor assessment; remaining architectural findings are not claims that the larger design has been implemented.

Reviewed the PMM facade, Gaf, AllocRequest, ZoneSelector, PmNode, PmZone, FrameBuddy, and the contiguous-VMO caller at commit `f7b22de46124aecda224e4cb07d0e077a55667ec`. Authorities: `kernel-design` and its MM reference, `modern-cpp-development`, `codebase-design`, `.clang-format`, and `.clang-tidy`. Compared the public allocation contracts with Zircon's `pmm_alloc_page`, `pmm_alloc_pages`, and `pmm_alloc_contiguous` in the local Fuchsia checkout. This review retains the project's NUMA/zone topology, sparse-descriptor model, and upper-layer reclaim decisions.

Ran the repository's prebuilt LLVM 23 clang-format dry-run and clang-tidy against eight files: `pmm.hpp`, `gaf.hpp`, `priv/alloc_request.hpp`, `priv/zone_selector.hpp`, `pmm.cpp`, `pm_node.cpp`, `pm_zone.cpp`, and `frame_buddy.cpp`, using `out/vm-address`. Formatting passed. Tidy completed without compiler errors but emitted existing naming, nodiscard, and other warnings, including dependencies; this is not a warning-clean claim. Logs and manifest are in `/tmp/pmm-design-review/`.

A separate compile probe using the real PMM translation-unit flags reproduced the broken node-specific `get_frame_n<u8>` template. Other findings are source-based design judgments, not runtime reproductions. No kernel code was changed and no boot test was run for this review.

## What is done well

- [FrameBuddy::allocate/release](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/frame_buddy.cpp:21) is a compact mechanism: split, select, and merge, with no topology or reclaim policy. Preserve this boundary.
- [PmZone::alloc_frame](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/pm_zone.cpp:184) owns the lock around buddy operations and zeros returned memory outside that lock. Keep one owner of the free-list invariant; an additional buddy lock would duplicate ownership.
- [AllocRequest](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/include/ours/mm/priv/alloc_request.hpp:194) keeps the resolved request immutable and separates it from the selector's iterator position. Node fallback and zone filtering have a useful home.
- [PmNode::alloc_frames](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/pm_node.cpp:168) correctly keeps reclaim above PMM. VMO/page-queue operations belong to their owners; a synchronous reclaim call from inside the zone lock would invert that dependency.

## High severity

None established in this scoped review. In particular, the per-CPU cache branches are currently disabled by `MAX_PCPU_CACHE_ORDER = 0`; their placeholder bodies are not evidence of an active free-path leak.

## Medium severity

### M1. Clarify the block contract; page-count conversion already exists

The public allocation family in `kernel/mm/include/ours/mm/pmm.hpp:121` already provides count-to-order helpers: `alloc_frame_n` and `get_frames_n` call `get_order(n)`. `get_frames` itself accepts an order and converts the result to a direct-map pointer. Therefore, ordinary callers do not need to repeat the count-to-order calculation. The earlier conversational assessment overstated that gap.

For example, `alloc_frame_n(..., 3)` obtains four pages. This is a coherent contract if the operation promises a power-of-two block large enough for at least three pages; rounding is not itself a defect. The issue to settle is whether names and documentation consistently distinguish that contract from an exact count of independent pages. Fragment allocation adds another specialized contract: five pages are allocated as order 0 plus order 2, so it may fail even when five arbitrary pages are available.

The raw block pointer leaves the caller responsible for the matching order for `free_frames(frame, order)`. The contiguous-VMO caller at `kernel/mm/vm_object_paged.cpp:173` additionally combines size with an independent alignment, converts block ownership to individual-page ownership, and returns the surplus tail. Count-to-order helpers do not perform those operations. Their presence in VMO alone does not establish that all of them belong in PMM: converting to per-page VMO ownership can reasonably belong to VMO. A reusable constrained-contiguous allocation operation needs its own demonstrated contract before being added.

First name and document the existing block operation and its release/ownership transitions. Retain the existing count helpers where this rounded-block contract is what callers need. Add exact-count or independently aligned operations only where actual clients require those guarantees. Zircon's `pmm_alloc_pages(count, ...)` and `pmm_alloc_contiguous(count, ..., align_log2, ...)` are contract references rather than a requirement to copy its API.

### M2. Placement constraints and scarce-zone policy do not form a complete contract

[resolve_request](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/include/ours/mm/priv/alloc_request.hpp:230) resolves every public request to Dma through Normal. Internal `with_zone_range()` exists, but no production path exposes it to a caller. General allocation cannot express a physical-address ceiling or alignment independent of the buddy block size. Those are hard resource constraints for prospective device callers, unlike a soft preference for local memory.

[ZoneList local ordering](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/include/ours/mm/pm_node.hpp:59) appends local zones in ascending type order, and remote nodes repeat that ordering. The first-success walk consequently spends Dma before Dma32 and Normal for ordinary allocations. This is already recorded in `docs/repairs/261028/mm.md`, item 1; it is not a new discovery. No current zone-constrained client was established, so this is policy debt and a future resource-shortage risk, not a demonstrated driver failure.

Represent hard admissibility separately from preferences. Resolve node/zone/address constraints once, prefer ordinary eligible memory before scarce low memory, and define fallback/reserve policy deliberately. Do not export ZoneType as the only way for clients to describe a device's addressing limit.

### M3. Allocation flags and result types advertise inconsistent behavior

`Gaf::DirectReclaim` in the reviewed tree (now [alloc_flags.hpp](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/include/ours/mm/alloc_flags.hpp:13)) retains `DirectReclaim`, although the execution path does not consume the bit and deliberately performs no direct reclaim. `GAF_KERNEL` carries it. Calling it permission to block also does not currently produce different admission behavior: [Zone admission placeholder](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/pm_zone.cpp:168) still checks an unwritten flag; the selector has no active watermark admission. The planned selector admission gate is recorded in `.agents/tasks/pmm-layering/PLAN.md`, step 8.

[try_alloc_frames and wait](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/pmm.cpp:306) has a coherent separate protocol: return `ShouldWait`, drop caller locks, then wait for a single frame. Other entries return null or Status and reject CanWait only through a debug assertion. The resulting contract depends on choosing the right entry as well as the flags. Global pressure gating also answers a different question from whether an eligible node/zone has a usable block; a healthy global count does not establish local availability.

Rename flags around the behavior actually promised and make invalid combinations difficult to express. Distinguish immediate allocation, permission to defer, admission/reserve eligibility, and zeroing. Preserve the single-page wait protocol and upper-layer reclaim seam; arbitrary contiguous-allocation waiting is not promised by the Zircon counterpart either. Complete the per-zone admission policy without treating a precheck of approximate counters as a strict reserve guarantee.

### M4. Thread placement policy is on the allocation path without applying policy

[apply_thread_policy](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/pmm.cpp:385) takes the current thread's resource spinlock, copies its policy, and switches on Bind/Interleave/Preferred, but every branch does nothing. `priv/memory_allocation_policy.hpp:11` hard-defines `MEMORY_ALLOCATION_POLICY` to 1, while the reviewed build passes `MM_ALLOCATION_POLICY=false`. Thus the build setting does not disable this path. This establishes unnecessary work and a misleading extension boundary, not a measured performance regression or an active non-null-policy bug.

Keep the future policy scaffold, but connect enablement to the actual build setting. Define the boundary so policy resolves home node, allowed nodes, and fallback preference into an allocation request before the zone walk. A function that can only mutate Gaf cannot express the existing policy kinds adequately. Unsupported policy kinds should not silently claim successful application once policy installation becomes available.

## Minor and style

- Names should distinguish page count, block order, and contiguous extent before any larger refactoring. `alloc_frame_n`, `get_frames_n`, and node-specific `get_frame_n` currently make the family harder to predict. Expand `Gaf` or choose terminology that states allocation behavior when redesigning this contract; renaming alone would leave the semantic defects intact.
- [Node-specific get_frame_n](/Users/smallhuazi/.codex/worktrees/irq-final-review/ours.refactor.reasonix/kernel/mm/include/ours/mm/pmm.hpp:183) calls nonexistent `get_frame<T>` instead of an implemented helper. Instantiating `get_frame_n<u8>(NodeId{0}, GAF_KERNEL, 1)` fails with `use of undeclared identifier 'get_frame'`. Existing builds leave this template uninstantiated. Reproduce with the probe and log in `/tmp/pmm-design-review/`.
- Existing tidy warnings are secondary to the contract work. Empty cache/policy scaffolding is deliberate and is not a recommendation to delete those types.

## Teaching opportunities

None requested. The user asked for development-oriented design judgment and previously declined teaching exercises.

## Suggested follow-up order

1. Name and specify the allocation operations, quantity units, failure modes, and ownership/release transitions; fix the broken helper in that scoped change.
2. Carry physical/address/alignment constraints and placement preferences through a single resolved request; keep the buddy block interface as an explicit mechanism where needed.
3. Finish scarce-zone ordering and admission policy, acknowledging the existing layering plan. Validate fragmentation, rollback, constrained placement, and reserve behavior rather than only successful allocation.
4. Align policy enablement and behavior with the resolved request. Exercise NUMA preferences only when the policy implementation is present.
5. Retain FrameBuddy, zone lock ownership, and the upper-layer reclaimer. Add per-CPU caching when it has a real workload and accounting contract; it is not the first redesign target.
