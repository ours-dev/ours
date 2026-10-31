# pmm-layering: land the layering as a sequence of behaviour-neutral steps

**Date**: 2026-10-28
**Status**: Implemented

Steps 1, 2, 3 and 5 landed. Steps 4, 6, 7 and 8 are superseded by `.agents/tasks/numa-out-of-mm/PLAN.md`, which is one line of work for what is left: the NUMA split changes what three of them are — `Router` is moved rather than renamed, `PmNode::Current`'s per-CPU half is the same thing as that design's placement table, and the registry extraction is what cuts `srat.cpp`'s include. This document stays as the record of what it executed.

## Request

Redo the layering of `PmNode`, `PmZone` and `ZoneList`, after the first attempt at a watermark admission gate was rejected for being arbitrary. The design is `DESIGN.md` in this directory; this plan is how it lands.

Scope ends at the layering plus the admission gate that motivated it. It does not include the reclaimer, the free-memory signal, the zone walk's ordering (`docs/repairs/261028/mm.md` item 1), `lowmem_reserve`, or making the per-CPU frame cache real.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| P1. Where the work happens | (a) the `feat/pmm-layering` worktree; (b) the baseline worktree | (a) | None | **(a)**, 2026-10-28 |
| P2. Whether each step commits separately | (a) one step, one commit, boot-verified; (b) batch the mechanical ones | (a) — the signal this buys is that the test counts never move, and batching loses which step moved them | None | **(a)**, 2026-10-28 (D5 of DESIGN.md) |

The design's own decisions: D1 **(c)**, D2 **(a)**, D4 **as recommended**, all 2026-10-28. D6 is resolved by D1(c). D3 and D7 are open and are decided at the step that needs them, steps 6 and 3.

## Steps

1. **Delete the dead state.** `PmZone::m_frames` (no reader), `FrameSet` (folded into `FrameBuddy`), `PmNode::m_present_frames` and `m_reserved_frames` with their accessors, their two call sites and `dump()`'s node half; `PmNode::m_spanned_frames` becomes a local in `init()`. The per-CPU frame cache stays. Why now: it depends on none of the other decisions, it is the smallest step, and it takes the noise out of every diff that follows. Verified by identical boot counts.
2. **`AllocRequest` and `resolve_request()`.** One immutable value replaces `MemoryAllocationContext`, `AllocationOptions` and `gaf_to_allocation_control()`; `resolve_request()` absorbs `alloc_frames_preamble()` and `apply_thread_policy()`. The walk still uses `ZoneList::for_each_zone` and simply reads the request. Why now: every later step reads better once the request has a name, and this one changes no control flow. Verified by identical boot counts plus the first host test in this cluster, over `resolve_request()`.
3. **`ZoneSelector`.** The walk becomes a selector over `ZoneList` and the request; the node filter copied into `get_frame_from_zone_list()` goes; both of the zone-type bounds are read (**D7** is decided here). The dead `AllocationContext` is deleted, and `pm_node_test.cpp`'s existing cases become tests of the production path. Verified by identical boot counts and by those cases passing unchanged.
4. **`NodeRegistry`.** The static node table and state masks move to file scope behind the `pmm.hpp` functions that already present them. The dead includes in `sparse_memmap.cpp` and `priv/memmap.hpp` go with it. Verified by identical boot counts and by `object_cache.cpp` and `memmap_init.cpp` no longer including `pm_node.hpp`.
5. **`NumaTopology`.** Its own header. `srat.cpp` gains `numa_topology.hpp` but **keeps** `pm_node.hpp`, for one call: `PmNode::set_state(nid, PmNode::State::Possible)`. That is the registry, so the include edge only goes once step 4 has landed too — the first wording of this step claimed otherwise. Verified as step 4.
6. **`PmNode::Current` dissolves** (**D3** is decided here). The `PerCpu<PmNode *>` becomes an ordinary `CpuLocal` variable; the policy half is already inside `resolve_request()` by step 2, so this step only moves the per-CPU half. Verified by identical boot counts, and `cpu_local.cpp` stops including `pm_node.hpp`.
7. **`Router` is renamed** for what it does: it builds the fallback order once at boot and takes no part in an allocation. Mechanical.
8. **The admission gate**, in the selector. `PmZone::alloc()` loses its gate and becomes pure mechanism; the selector applies `watermark_ok()` with the request's mark; `Flags::BelowHighWmark` becomes a sticky hint, which is where the atomic flag word and its interleaving argument belong. This is the change that was rejected as step 2b and it lands last, as a consequence. Verified by a new kshell test, `pmzone.wmark.test` — the first thing in the tree to exercise a watermark decision.

## Files

- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet` and `m_frames` go (1); the gate goes and `watermark_ok()` stays (8)
- `kernel/mm/pm_zone.cpp` — `FrameSet` call sites become `FrameBuddy` (1); `alloc_frames()` becomes mechanism only (8)
- `kernel/mm/include/ours/mm/pm_node.hpp` — counters and accessors go (1); registry statics leave (4); `Current` leaves (6); `Router` renamed (7)
- `kernel/mm/pm_node.cpp` — `dump()`'s node half (1); the walk becomes the selector (3)
- `kernel/mm/pm_node_init.cpp` — `m_spanned_frames` becomes a local (1); `Router` renamed (7)
- `kernel/mm/pm_node_topology.cpp` — becomes `NumaTopology`'s implementation (5)
- `kernel/mm/include/ours/mm/alloc_request.hpp` — new (2)
- `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — new, replacing `priv/zone_iterator.hpp` (3)
- `kernel/mm/include/ours/mm/priv/memory_allocation_context.hpp` — deleted (2)
- `kernel/mm/include/ours/mm/gaf.hpp` — `AllocationOptions` deleted (2)
- `kernel/mm/include/ours/mm/numa_topology.hpp` — new (5)
- `kernel/mm/memmap_init.cpp`, `kernel/mm/pmm.cpp` — the node-side reserved-frame calls go (1)
- `kernel/mm/sparse_memmap.cpp`, `kernel/mm/include/ours/mm/priv/memmap.hpp` — dead includes go (4)
- `kernel/arch/x86/mm/srat.cpp` — includes `numa_topology.hpp` instead (5)
- `kernel/mm/tests/alloc_request_smoke.cpp` — new host test (2)
- `kernel/commands/pm_zone_test.cpp` — `pmzone.wmark.test` (8)
- `docs/changes/<date>/mm.md` — one entry per step

## Test plan

The standing check at every step, with the counts that must not move:

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  148 passed, 0 failed
```

Steps 1 and 3 through 7 expect exactly those numbers. Step 2 raises the host-test count by the cases of `alloc_request_smoke`. Step 8 is the only step allowed to change kernel behaviour, and it is the only one with a test of its own: `pmzone.wmark.test` drains one zone to between its `Min` and `Low`, checks that a blocking allocation is refused there while a non-blocking one succeeds, drains below `Min` and checks both are refused, then gives the frames back and checks the zone's free count and free-block profile return to where they started.

Run from an idle shell, for the reason `pm_zone_test.cpp` already documents: the counts are read without holding the zone across both readings.
