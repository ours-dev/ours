# numa-out-of-mm: one line of work for what is left of the node layer

**Date**: 2026-10-28
**Status**: Proposed

## Request

The user's two instructions, 2026-10-28: mm should know nodes and not NUMA (`DESIGN.md` in this directory), and the two task plans should become one line of work rather than two that interleave on the same files.

So this plan replaces what is left of `.agents/tasks/pmm-layering/PLAN.md`. Steps 1, 2, 3 and 5 of that plan have landed; its steps 4, 6, 7 and 8 are superseded here, because the NUMA split changes what three of them are: `Router` is moved rather than renamed, `PmNode::Current`'s per-CPU half is the same thing as this design's node-placement table, and the registry extraction is what finally cuts `srat.cpp`'s include of the allocator.

Scope ends at the admission gate that started all of this. It does not include the reclaimer, the free-memory signal, `EarlyMem::set_node()` (D5), the zone walk's ascending order, or anything else in `docs/repairs/261028/mm.md`.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| P1. Where the rename sits in the sequence | (a) first, so every later step is written in the final vocabulary; (b) last, so the churn lands on finished code | (a). It is mechanical and behaviour-neutral, and (b) means writing new code in names that are about to change, then changing them | None | **(a)**, 2026-10-28 |
| P2. What replaces the two plans | (a) this plan is the forward line; `pmm-layering/PLAN.md` stays as the record of what it executed, with its remaining steps marked superseded; (b) fold everything back into `pmm-layering/PLAN.md` | (a). A plan that is partly executed is a record of that execution; overwriting it loses which steps ran under which reasoning | None | **(a)**, 2026-10-28 |

`DESIGN.md`'s own decisions: D1 **(a)** `kernel/lib/numa`, D2 **(a)** mm keeps its own placement table, D3 **(a)** the rename — all 2026-10-28. D4 and D5 are open; D4 is settled by step 2 below taking the forward route. `pmm-layering/DESIGN.md`'s D3, what becomes of `PmNode::Current`, was decided **(a)** on 2026-10-28 as part of step 3 here.

## Steps

1. **The vocabulary stops saying NUMA.** `NnId` → `NodeId` (162 occurrences, tree-wide, declared in `system/public/ours/types.hpp`), `NUMA_MAX_NODE` → `MAX_NODES`, `NUMA_NO_NODE` → the node layer's own sentinel. Mechanical and behaviour-neutral. Why first: every later step then writes the final names once instead of twice. Verified by identical counts, and by `git grep NnId` coming back empty.
2. **`kernel/lib/numa` exists and owns the tables.** Scaffolded with copier from `tools/templates/cpp_library`. `NumaTopology` — the distance matrix and the firmware placement — moves there out of `ours/mm/`, which is the forward half of **D4**: `ad246ae5` moved it out of `PmNode` and left it in mm, and this moves it out of mm. mm still calls `cpu_to_node()` across the seam at this step; step 3 removes that. `srat.cpp` includes `numa/topology.hpp` instead of `ours/mm/numa_topology.hpp`. Verified by identical counts plus `pmnode.test`.
3. **mm gets its own placement table, and `PmNode::Current` dissolves.** `NODE_OF_CPU` in node vocabulary (**D2(a)**), written at bring-up from what `kernel/lib/numa` says and by mm itself on a run-time move; `current_node()` reads it. `PmNode::Current`'s per-CPU `PmNode *` becomes `NodePlacement`'s cache of that table, its `apply_thread_policy()` and `local_node_for()` go to `pmm.cpp` beside their only callers, and its CPU-claiming half becomes `PmNode`'s own statics (**D3(a)** of `pmm-layering/DESIGN.md`). Done in two commits: the table at `c90e86b5` (3a), the dissolution (3b). This is the step after which **mm makes no call into `kernel/lib/numa` on any path**. Verified by identical counts, `pmnode.test`, and `cpu_local.cpp` no longer including `pm_node.hpp`.
4. **The cut: the fallback order is computed outside mm.** `Router::build()` moves to `kernel/lib/numa` as the derivation of a per-node nearest-first order and per-node CPU set. `ZoneList::connect()` takes an ordered list of node ids instead of a `NodeMask` and appends without sorting. `init_pmm()` and `PmNode::bootstrap()` take the orders and the CPU sets. mm has no distance reader left. The plan's own check here — `ZoneList::dump_fallback()`'s boot output being identical to the step before — turned out to be unobtainable: the dump goes to `log::debug` from `bootstrap()`, early enough that nothing reaches the serial log, and the default build's `MAX_NODES == 1` leaves no remote hops to compare. Verified instead by eight host cases over matrices the test writes itself (`//kernel/lib/numa/test:fallback_order`), `pmnode.test` against this machine's masks, and `git grep Topology::distance -- kernel/mm` coming back empty. Done, 2026-10-29; see `docs/changes/261029/mm.md`.
5. **`NodeRegistry`.** The node table and state masks leave `pm_node.hpp` for file scope behind the `pmm.hpp` functions that already present them. The dead includes in `sparse_memmap.cpp` and `priv/memmap.hpp` go. `srat.cpp` stops including `pm_node.hpp` at last, which needs this step and step 2 together. Verified by identical counts and by `object_cache.cpp`, `memmap_init.cpp` and `srat.cpp` no longer including it.
6. **The admission gate**, in the selector. `PmZone::alloc_frames()` loses its gate and becomes mechanism only; `ZoneSelector::admits()` applies `watermark_ok()` with the request's mark; `Flags::BelowHighWmark` becomes a sticky hint, which is where the atomic flag word and its interleaving argument belong. This is the change rejected as "step 2b" a day ago and it lands last, as a consequence. Verified by a new kshell test, `pmzone.wmark.test`.

## Files

- `system/public/ours/types.hpp`, `kernel/mm/include/ours/mm/cfg.hpp` — the renamed type and constants (1)
- everything that names them, tree-wide — 162 `NnId` sites (1)
- `kernel/lib/numa/` — new module: `topology.hpp`/`.cpp`, then the ordering derivation (2, 4)
- `kernel/mm/include/ours/mm/numa_topology.hpp`, `kernel/mm/numa_topology.cpp` — deleted, their contents moved (2)
- `kernel/mm/include/ours/mm/node_placement.hpp` — new (3)
- `kernel/mm/include/ours/mm/pm_node.hpp` — `Current` (3), `Router` (4), the registry statics (5)
- `kernel/mm/pm_node.cpp`, `kernel/mm/pm_node_init.cpp`, `kernel/mm/init_pmm.cpp` — the bring-up parameters (4), the registry (5)
- `kernel/mm/include/ours/mm/node_registry.hpp`, `kernel/mm/node_registry.cpp` — new (5)
- `kernel/arch/x86/mm/srat.cpp` — includes `numa/` (2), stops including `pm_node.hpp` (5)
- `kernel/mm/sparse_memmap.cpp`, `kernel/mm/include/ours/mm/priv/memmap.hpp` — dead includes (5)
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp`, `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — the gate (6)
- `kernel/commands/pm_node_test.cpp` — follows each step; `kernel/commands/pm_zone_test.cpp` gains `pmzone.wmark.test` (6)
- `kernel/lib/numa/test/` — a host test over the ordering derivation (4)
- `docs/changes/<date>/mm.md` — one entry per step

## Test plan

The standing check at every step, with the numbers that must not move:

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

and, because `boot_check.py` does not run them, `python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test` both `ok`. Steps 1 through 5 expect exactly those; step 4 raises the host-test count by the ordering derivation's test, and step 6 is the only step allowed to change kernel behaviour.

Step 4 carries one extra check that the counts cannot give: `ZoneList::dump_fallback()` already prints each node's fallback order at boot, so capturing it before and after must give the same sequence. On the single-node dev guest that is weak — the order is the node's own two zones — so the real one is `mm_numa = true`, which is what `pm_node_test.cpp`'s remote-zone cases were written for.

The whole task's acceptance, after step 6: `git grep -i numa kernel/mm` returns nothing but prose in change records.
