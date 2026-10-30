# zone-table: the zones are cut into a table before any node exists, and each node claims its row of it

**Date**: 2026-10-29
**Status**: Proposed

## Problem
Each `PmNode` cuts its own zones in `PmNode::init()` (`kernel/mm/pm_node_init.cpp:281-299`) and keeps them in its `ZoneList` by type. "Every zone in the machine" has no owner of its own: `PmZone::for_each()` walks a side list that the zone's constructor appends to (`3c11c310`), a second record of what the nodes' arrays already hold.

The user, 2026-10-29: the scheme that divides memory into zones is known statically, so cut the zones first and build the nodes after. What is static is the rule, not the result: the type boundaries come from the platform's list (`kernel/arch/x86/kernel/include/ours/arch/mem_cfg.hpp:23`, DMA below 16 MiB, DMA32 below 4 GiB, Normal above), while which zones exist depends on how much memory each node has, which the boot memory map says. That map, with each range's node, is read before `init_pmm()` runs, so the zones can be cut before any `PmNode` is made.

## Goals and non-goals
Goals:

- A table the zones own: one cell for each node and zone type, `NUMA_MAX_NODE` rows by `NR_ZONES_PER_NODE` columns of `PmZone *`. An empty cell is a node with no memory of that type. Filled while the PMM is brought up, before any `PmNode` exists.
- `PmZone::for_each()` walks the table, and `PmZone::at(nid, type)` reads one cell; the side list of `3c11c310` goes.
- Each zone's structure stays in its own node's memory, as now.
- A node no longer makes zones. Its `ZoneList` keeps the fallback order only; "the node's zone of type T" is the table's cell.
- Nothing counted changes: the same zones, bounds, managed frames, watermarks and fallback order.

Non-goals:

- Memory hotplug. The table leaves room for it (a range added later fills an empty cell; a new node fills an empty row), but a zone that grows needs its `FrameSet` to grow, which is not designed here.
- Merging the unmerged `feat/pmm-cpp-vocabulary`, which rewrites `pm_zone.hpp`, `pm_zone.cpp`, `pm_node.hpp` and `pm_node_init.cpp` too.

## Design
The table, with the example of a machine of two nodes split at 3 GiB:

```text
            DMA      DMA32     Normal   Movable  Dynamic
node 0    [zone]    [zone]    [    ]    [    ]   [    ]
node 1    [    ]    [zone]    [zone]    [    ]   [    ]
...       (NUMA_MAX_NODE rows: 32 with NUMA, 1 without; 160 pointers at most)
```

Bring-up, `PmNode::bootstrap()`, in this order:

1. `find_possible_nodes()`, as now: which nodes own memory.
2. The type bounds, from the platform's list, as `set_zone_bounds()` does now, with repair item 2 of `docs/repairs/261029/mm.md` fixed: a type the platform does not list gets an empty range.
3. **New**: `PmZone::create_all()`. For each node with memory and each type, the node's range cut to the type's bounds; a non-empty cut gets a `PmZone`, allocated from that node's memory (`EarlyMem::allocate<PmZone>(1, nid, ...)`, which searches only that node's ranges, `kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp:651`), and goes into its cell. It needs the node's number, not its `PmNode`.
4. `create_nodes()` and each node's `init()`, as now, but making no zone.
5. `connect_nodes()`: each node's fallback order is its own row, then the other nodes' rows, nearest first, as now.

Who reads what:

- `PmZone::for_each()` walks the table row by row, skipping empty cells: the two free counts in `pmm.cpp` and `PmNode::setup_watermarks()`.
- `PmZone::at(nid, type)` replaces `ZoneList::local_zone(type)`: the free path (`pm_node.cpp:318`, which then needs no node at all, only the frame's node and zone numbers), the memmap's initialisation, the dump, the tests.
- `ZoneList` keeps the fallback order; `for_each_local_zone()` and `local_zones()` read the node's row; `insert_local_zone()` and its by-type array go.

No lock: the table is written on the boot CPU, before the other CPUs run and before anything reads it, and never after, since no zone is ever destroyed or added. The structures the cells point at are the hot part (free lists, locks, counters), each in its own node's memory; the table itself is about 1 KiB, read-only, and sits in every CPU's cache without anyone invalidating it.

## Alternatives considered
- **A compact list of the zones that exist, each node keeping where its run starts and ends.** Saves about 1 KiB, but a lookup by node and type walks the node's run, and a zone added later would have to be inserted in the middle. Rejected by the user's choice of the fixed grid, 2026-10-29.
- **Zones kept inside the nodes, with the side list of `3c11c310`.** Two records of the same zones, kept in step by the constructor.
- **The table in a class of its own.** The user asked, 2026-10-29, for `PmZone` to provide the walk; the table is the zones' own, so it stays a static of `PmZone`, written by `create_all()` alone.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. The table's shape | (a) a fixed grid, nodes by types; (b) a compact list, each node with its start and end | (a) | None (homework paused) | (a), 2026-10-29 |
| D2. `PmZone::m_node`, which only the constructor writes (to null), though its comment says the node fills it in when it adopts the zone | (a) filled when the node claims its row; (b) removed | (a): a field kept for planned work is not deleted as cleanup, and the claim is where it can be filled | None | |
| D3. The per-type node states `State::Dma`, `Dma32`, `Normal`: written for each zone, read by nothing, and numbered so that a Movable zone would set `State::Memory` and a Dynamic one `State::Online` (repair item 3) | (a) kept, set by `create_all()`, with numbers of their own; (b) removed: the table's cell answers whether a node has memory of a type | (b): the table is then the one record of which node has which zone | None | |

## Testing and verification
- `boot_check.py` for both images; the zones in the boot log (`Zone[...]: managed ... watermarks ...`) the same as before the change, and the free memory the watchdog logs at boot the same.
- `pmnode.test` and `pmzone.test`, which check a node's own zones and its fallback order, adapted where they read `ZoneList`'s by-type array; `mempressure.test`, `largepage.test`, `pagesource.test`; the host tests.
- A control: a node whose cells are not read by the free path (`PmZone::at()` answering null) panics or fails a test.
- More than one node: the default build has `mm_numa = false` (`kernel/kernel_params.gni:36`), so one row. A build with `mm_numa = true`, booted with `NUMA=1` (four nodes, `scripts/qemu_boot.py`), fills several rows; whether that configuration boots on this branch today is not known yet, and is checked before the table is built.

## Open questions
None
