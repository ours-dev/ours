# zone-table: steps from zones made by each node to a table of zones made before the nodes

**Date**: 2026-10-29
**Status**: Accepted

## Request
The user, 2026-10-29: cut the zones first and build the nodes after, since the scheme that divides memory into zones is static; `PmZone` provides the walk over every zone. The table is a fixed grid (D1), `PmZone::m_node` is filled when a node claims its row (D2), and the per-type node states go (D3). Scope ends at bring-up: memory hotplug is not designed.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. The table's shape | see DESIGN.md | (a) | None (homework paused) | (a), 2026-10-29 |
| D2. `PmZone::m_node` | see DESIGN.md | (a) | None | (a), 2026-10-29 |
| D3. The per-type node states | see DESIGN.md | (b) | None | (b), 2026-10-29 |

## Steps
Each step is introduced to the user before it starts, and lands in its own commit.

1. **The zone bounds read past the platform's list.** Repair item 2 of `docs/repairs/261029/mm.md`, and the shift found with it: the platform passes its bounds as a short list by position, so the kernel reads two entries past it, and a type the platform leaves out would shift every later bound down one type. The platform fills one bound per zone type, by type, 0 for a type it does not list; `set_zone_bounds()` asserts it has one for each. Why first: step 2 moves this code. Verified by the zones in the boot log, unchanged. Done: the zones unchanged; a short list stops the boot on the new assertion; with DMA switched off the old code called all 126280 frames DMA, the new one DMA32.
2. **The table, made before the nodes.** `PmZone::create_all()`, `PmZone::at()`, `PmZone::for_each()` over the table; `PmNode::init()` makes no zone; the side list of `3c11c310` and the per-type node states (D3, repair item 3) go. Checked first: whether a build with `mm_numa = true` boots with `NUMA=1`. Verified by the boot log's zones, the PMM tests, and a control. Done: the NUMA build boots with four nodes, before and after; the zones of both builds and the four nodes' fallback orders the same as before; the PMM tests pass on both; a cell left empty stops the boot.
3. **The node claims its row.** `ZoneList` keeps the fallback order only and reads its own zones from the table; `PmZone::m_node` is filled at the claim (D2); the free path asks `PmZone::at()`. Verified as step 2. Done: the zones of both builds and the four nodes' fallback orders as before (node 3's Normal zone manages 2 frames fewer in the NUMA build, the kernel image a page larger); the PMM tests pass on both; `PmZone::at()` answering null for DMA32 stops the boot on the free path's assertion.
4. Later steps are introduced one at a time.

## Files
- `kernel/platform/pc/memory.cpp`, `kernel/mm/pm_node_init.cpp`, `kernel/mm/include/ours/mm/init.hpp` — step 1
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp`, `kernel/mm/pm_node_init.cpp`, `kernel/mm/include/ours/mm/pm_node.hpp` — step 2
- `kernel/mm/pm_node.cpp`, `kernel/mm/memmap_init.cpp`, `kernel/mm/pmm.cpp`, `kernel/commands/pm_node_test.cpp`, `kernel/commands/pm_zone_test.cpp` — step 3

## Test plan
- Every step: `boot_check.py`, both images; `Zone[...]: managed ... watermarks ...` in the boot log the same as before the task.
- Steps 2 and 3: `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test`; a multi-node boot if the NUMA build boots.
