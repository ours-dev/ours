# numa-out-of-mm: mm knows nodes, and NUMA is one thing that can fill them

**Date**: 2026-10-28
**Status**: Proposed

## Problem

mm is built on NUMA rather than on nodes. The user's words for what it should be instead: a node is the abstraction, and NUMA is something a node can accommodate.

What that costs today, concretely. `PmNode` is documented as "a class that describes a NUMA domain". The id type is `NnId`, the ceiling is `NUMA_MAX_NODE`, the absent-node sentinel is `NUMA_NO_NODE`, and `NodeMask` is a bitset sized by `NUMA_MAX_NODE`. Until `ad246ae5` a distance matrix and two firmware-placement tables were nested inside `PmNode` itself; that commit moved them to `ours/mm/numa_topology.hpp`, which took them out of the node class and left them in mm under a NUMA name — the opposite of what is wanted, and the immediate reason for this document.

The encouraging half is that the dependency is shallower than the names suggest. A census of every consumer of distance and firmware placement:

| Consumer | What it uses | Where it belongs |
|---|---|---|
| `kernel/arch/x86/mm/srat.cpp` | `set_distance()`, `set_cpu_node_early()` | the source; already arch |
| `PmNode::Router::build()` | `distance()` — the **only** distance reader in mm | out of mm |
| `PmNode::init()` | `firmware_node()`, to build a node's hard CPU affinity | out of mm |
| `PmNode::Current::resolve_local_node()`, `current_node()` | `cpu_to_node()` | stays reachable, in node vocabulary |
| `kernel/commands/pm_node_test.cpp` | all of the above | follows whatever the code does |

**The allocation path consumes no NUMA knowledge at all.** Distance is read once, at bring-up, by one function, to sort the fallback order; after that every allocation walks the sorted list and never asks again. Nothing outside mm reads a node id either — `Cpu::nid()` was deleted for having no callers and always answering zero, so `srat.cpp` is the only writer and mm is the only reader.

So the split is possible and the seam is narrow. What has been missing is a module to own the NUMA half: ACPI's SRAT and SLIT parsing already lives in `kernel/lib/acpi` (`acpi/numa.hpp`, `acpi/structures/slit.hpp`), and its *results* were put in mm. There is nothing in between.

## Goals and non-goals

Goals:

- mm's node layer knows: a node's frames, its zones, its CPU set, and an ordered list of nodes to fall back to. Nothing about distance, nothing about firmware.
- A module outside mm owns NUMA: the distance matrix, the firmware placement, and the derivation of a per-node fallback order and per-node CPU set from them.
- The derivation happens once, at bring-up, and crosses into mm as data. mm never calls into the NUMA module on an allocation.
- A machine with no NUMA has exactly one node and nothing special happens: the same code path, a one-element order.
- mm's vocabulary stops saying NUMA. `NnId` becomes `NodeId`, `NUMA_MAX_NODE` becomes `MAX_NODES`, `NUMA_NO_NODE` becomes the node layer's own sentinel.

Non-goals:

- Removing nodes, or making the single-node case a separate path. One node is the general case with one element.
- Changing what any allocation returns, or the order in which zones are walked on this machine. The order is computed somewhere else and comes out the same.
- Rewriting ACPI parsing. `kernel/lib/acpi` already does it; this gives its results a home.
- Fixing the ascending zone-walk order (`docs/repairs/261028/mm.md` item 1) or anything else already filed.

## Design

### The three layers and what crosses between them

**arch** — reads the firmware. On x86 that is `kernel/arch/x86/mm/srat.cpp` over `kernel/lib/acpi`. It writes into the NUMA module and nothing else: distances, and which node each CPU is on.

**`kernel/lib/numa`** — owns NUMA. The distance matrix, the firmware CPU placement, and the two derivations mm needs: for each node, the other nodes in nearest-first order, and the set of CPUs on it. `PmNode::Router::build()` moves here, because sorting nodes by distance is what it is. Nothing platform-specific: every platform with NUMA sorts the same way, which is why this is not `kernel/platform/<board>`.

**mm's node layer** — receives, once, per node: its CPU set and its ordered fallback list. It keeps its own CPU-to-node table so that "which node is this CPU on" is answerable before any of mm's per-CPU state exists, which is a real constraint today (`PmNode::Current`'s per-CPU pointer is not filled until an init hook runs, while allocation starts asking much earlier). That table is written at bring-up from what the NUMA module says and updated by mm itself when a CPU moves between nodes at run time.

So the crossing is one way, once, at bring-up, and it carries data rather than questions.

### Interfaces

Out of mm, into `kernel/lib/numa`:

- the distance matrix and `distance(x, y)`
- the firmware placement and `firmware_node(cpu)`
- `Router::build(from, candidates)`, renamed for what it does: derive a nearest-first order
- a derivation per node: `cpus_of(node)` and `fallback_order(node)`

Staying in mm, in node vocabulary:

- `NODE_OF_CPU`, a `std::array<NodeId, MAX_CPU>` written at bring-up and on a run-time move; `current_node()` reads it
- each `PmNode`'s own CPU masks and its `ZoneList`
- `ZoneList::connect()`, which takes an **ordered list of node ids** instead of a `NodeMask`, and appends each one's zones without sorting anything

### Events, before and after

Before:

```text
1. srat.cpp fills DISTANCE and the firmware placement
2. PmNode::bootstrap() asks every node to connect()
3. connect() calls Router::build(), which reads DISTANCE and sorts   <-- in mm
4. connect() appends each node's zones to m_fallback in that order
5. every allocation walks m_fallback and never looks at DISTANCE again
```

After:

```text
1. srat.cpp fills the NUMA module
2. the NUMA module derives, per node, a nearest-first order and a CPU set
3. init_pmm() is handed those, and PmNode::bootstrap() passes them down
4. connect() appends each node's zones in the order it was given
5. every allocation walks m_fallback, exactly as before
```

Step 3 of the old sequence is the whole change: it moves out of mm, and what used to be a question mm asked becomes a parameter mm receives.

### Reference

Linux is the model and it is already this shape. `pg_data_t` (`include/linux/mmzone.h`) holds zones, kswapd and the LRU; it holds no distance matrix. The distances are `numa_distance` in `arch/x86/mm/numa.c` behind `node_distance()`, the CPU placement is `numa_cpu_node` there behind `cpu_to_node()`, and `build_zonelists()` in `mm/page_alloc.c` consumes `node_distance()` once at bring-up to order each node's zonelist — after which the allocator walks the zonelist and never asks again. The one place this design departs: Linux's `cpu_to_node()` stays in arch and mm calls it, where this keeps a node-vocabulary table inside mm (see D2).

Zircon has no counterpart: one `PmmNode` per machine, no zones, and CPU topology is not a VM concern.

## Alternatives considered

**Leave the tables in mm behind a NUMA-named header** — what `ad246ae5` did. Rejected by the user: it moves the code without moving the concept, and mm still contains a thing called NUMA.

**Give the derivation to the platform layer** — my first answer, and wrong. Sorting nodes by distance has no platform specificity: every platform with NUMA does the same thing, so `kernel/platform/pc` would mean the next platform copies it. The matrix is filled by arch, not platform; `platform_init_pmm()` is merely where the call chain passes through, which is a position in a sequence and not an owner.

**Put the derivation in arch** — closer, since arch already fills the matrix. Rejected because the derivation is not architecture-specific either: SRAT/SLIT parsing is x86's, sorting by distance is not. Arch would duplicate the sort per architecture for the same reason platform would.

**Have mm ask the NUMA module at run time** instead of keeping its own table. Simpler, one fewer copy. Rejected, see D2: it puts a dependency on a NUMA module on the allocation path, which is precisely the thing being removed, and it does not answer the bring-up ordering problem any better.

**Delete `NodeMask` in favour of the ordered list.** Tempting, since the order supersedes the mask for the fallback walk. Rejected: the mask is still what an allocation request carries to say which nodes it may draw from (`AllocRequest::allowed_nodes`), which is a different question from the order they are tried in.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the NUMA derivation lives | (a) `kernel/lib/numa`, a module of its own; (b) `kernel/platform/<board>`; (c) `kernel/arch/<arch>/mm` | (a). Sorting by distance is neither platform- nor architecture-specific, so (b) and (c) both duplicate it per board or per arch. `kernel/lib/acpi` already holds the parsing half, and (a) is the same shape | None (homework paused) | **(a)**, 2026-10-28 |
| D2. How mm answers "which node is this CPU on" | (a) mm keeps its own `NODE_OF_CPU` table in node vocabulary, written at bring-up from the NUMA module and on a run-time move; (b) mm calls the NUMA module each time | (a). (b) puts a NUMA dependency on the allocation path, which is what this removes, and the table has to exist in some form anyway: mm must answer this before its own per-CPU state is filled | None | |
| D3. Naming | (a) `NnId` → `NodeId`, `NUMA_MAX_NODE` → `MAX_NODES`, `NUMA_NO_NODE` → the node layer's sentinel, tree-wide; (b) leave the names | (a) | None | **(a)**, 2026-10-28 |
| D4. What happens to `ad246ae5` | (a) a forward commit splits `NumaTopology` in two; (b) rewrite the branch so that commit never existed | (a). The branch is unpushed so (b) is available, but that commit's record explains why nesting the tables in `PmNode` was wrong, and that reasoning is worth keeping even though its placement was only half right | None | |
| D5. `EarlyMem::set_node()` | (a) in scope: `srat.cpp` also tells the boot allocator which node a memory range is on, which is the other NUMA-to-mm edge; (b) out of scope for now | (b). It is an edge of the same kind, but it crosses into the boot allocator rather than the node layer, and folding it in would double this task | None | |

## Testing and verification

The standing check, at every step: `ours-verify` with counts identical to the step before — `dbg 44 passed, 4 skipped`, `rel 41 passed, 8 skipped, 6/6 required`, `host-tests 149 passed` — plus `scripts/kshell_run.py --out <dir> pmnode.test pmzone.test` both `ok`, because `boot_check.py` does not run the kshell tests and `pmnode.test` is what exercises this code.

The substantive check is that the zone walk comes out in the same order. `ZoneList::dump_fallback()` already prints a node's fallback order at boot; capturing it before and after must give the same sequence. On the single-node dev guest that is a weak check — the order is the node's own two zones — so the real one is a NUMA QEMU configuration, which `pm_node_test.cpp`'s remote-zone cases are written for and which `mm_numa = true` builds.

`kernel/lib/numa`'s derivation is pure: a distance matrix in, an order out. That makes it the second thing in this area a host test can reach, in the shape of `kernel/mm/tests/alloc_request_smoke.cpp`. Worth pinning: a node is never in its own order, every other node with memory appears exactly once, ties break deterministically, and an empty candidate set yields an empty order — which is what `pm_node_test.cpp` checks today against the version inside mm.

What cannot be verified: that mm no longer "perceives" NUMA. That is a property of the names and the include graph, and the check for it is mechanical — after the last step, `git grep -i numa kernel/mm` should return nothing but prose in change records.

## Open questions

- Whether `NodeMask`'s width should still be a NUMA-derived constant. Renaming `NUMA_MAX_NODE` to `MAX_NODES` fixes the word, but the value is still "how many NUMA domains the build supports". If nodes are the general concept, the ceiling is a node-layer configuration and `mm_numa = false` should mean "one node", not "no NUMA" — which is what it already means in effect, since `NUMA_MAX_NODE` is then 1.
- Whether `PmNode::Current` survives this. It is `PmNode::Current::set_nid()` that moves a CPU between nodes at run time, and under D2 that write lands in mm's own table. That overlaps step 6 of `.agents/tasks/pmm-layering/PLAN.md`, which was going to dissolve `Current` anyway; the two should probably be one step.
- Whether anything other than the fallback order will ever want the distance matrix. If the scheduler grows NUMA-aware placement it will, and then `kernel/lib/numa` is already the right place for it to ask — which is an argument for (a) in D1 beyond the duplication one.
