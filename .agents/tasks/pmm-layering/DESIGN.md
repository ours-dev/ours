# pmm-layering: give PmNode, PmZone and ZoneList one job each, and the allocation request a name

**Date**: 2026-10-28
**Status**: Proposed

## Problem

Every placement question in the physical allocator currently has no principled answer. The one that forced this was small — where does a watermark admission check go? — and the answer turned out to be unobtainable: the check is a policy derived from the caller's request, there is no object representing a request, so the check had to be parked inside the object that happens to own the free lists, and then a reason had to be invented for it. That design was written and rejected, correctly.

The underlying state of the cluster, with the evidence:

**One allocation request has four representations, and the best one is dead.**

| Representation | Holds | On the real path? |
|---|---|---|
| `Gaf` | the caller's intent: `DirectReclaim`, `ThisNode`, `Zero` | yes |
| `AllocationOptions` + `gaf_to_allocation_control()` | the policy derived from that intent | threaded through three signatures, always the empty set |
| `MemoryAllocationContext` | `{initiator, allowed_nodes, home_node}` | yes, filled by `alloc_frames_preamble()` |
| `AllocationContext` + `ZoneIterator` | the request *and* its zone walk | **no** — only `kernel/commands/pm_node_test.cpp` |

Two types whose names differ by one word. The one with the better shape — a request that owns its own walk — was abandoned, and its own test says so: "ZoneIterator and AllocationContext. Rewritten with ZoneList and not on the allocation path yet, so nothing else exercises them." What replaced it copies out by hand the node filter that `ZoneIterator::move_next()` already implements:

```cpp
// pm_node.cpp:143                        // priv/zone_iterator.hpp:65
if (!context.allowed_nodes.test(          if (nodes && !nodes->test(
        zone.which_node())) {                     zone->which_node())) {
    return nullptr;                           continue;
}                                         }
```

**`PmNode` is seven things.** It is at once the NUMA domain descriptor (`m_id`, `m_start_pfn`, affinity masks), the registry of which nodes exist and in what state (`static NODES`, `STATES`, `NUM_NODES` and the dozen statics over them), the machine's CPU-to-node topology (`PmNode::Topology`, a distance matrix and two `MAX_CPU` arrays), the per-CPU current-node accessor and thread-policy applier (`PmNode::Current`), the fallback-path algorithm (`PmNode::Router`), the allocation front door, and the owner of the reclaim ranking (`PageQueues`).

**Both references keep three of those outside the node.** Linux's `pg_data_t` holds zones, kswapd and the LRU, and nothing else from that list: the registry is the plain globals `node_data[]` and `node_states[]` in `mm/page_alloc.c`, the topology is `cpu_to_node()` and `numa_cpu_node` in `arch/x86/mm/numa.c`, and the per-CPU current node is the `numa_node` per-CPU variable. Zircon has no node class to overload — `Pmm` holds the single `PmmNode`, and CPU topology is not a VM concern at all.

**The public interfaces are far larger than their one caller needs.** Outside `kernel/mm/` and the two kshell tests, exactly one file in the tree includes `pm_node.hpp` or `pm_zone.hpp`: `kernel/arch/x86/mm/srat.cpp`, and it uses only `PmNode::set_state()` and three `PmNode::Topology::set_*()` calls. Every other user of physical memory — roughly 180 call sites — goes through the free functions in `pmm.hpp`. So `PmNode` (about 25 public methods plus three nested classes) and `PmZone` (about 20 public methods) are *implementation*, and in the skill's terms they are shallow: each interface is nearly as complex as what sits behind it, for a single internal caller.

**There is dead and duplicated state behind those interfaces.** `PmZone::m_frames`, a `std::array<PmFrame::List, NR_FRAME_ORDERS>`, has no reader at all — the free lists live in `m_fset`. `PmZone`'s per-CPU frame cache is unreachable: `MAX_PCPU_CACHE_ORDER` is 0, so `is_order_within_pcpu_cache_limit()` is always false and both `with_current` lambdas (which already do nothing) are dead code. `PmNode::m_spanned_frames`, `m_present_frames` and `m_reserved_frames` duplicate the sum of the zones' own counters and are read only by `PmNode::dump()`, which has no caller. `FrameSet` is five methods over `FrameBuddy`, three of them one-line forwards, with exactly one user.

Why now: `83107ad1` landed watermarks with no consumer, and the obvious consumer cannot be placed until this is sorted. Carrying on would add a second unlanded step on top of the first.

## Goals and non-goals

Goals:

- One name for an allocation request, carrying the policy derived from it, replacing four representations with one.
- One job per class: `PmZone` is a range of frames with a free-list mechanism and its own admission state; `PmNode` is a NUMA domain and its zones; the registry, the topology and the per-CPU current node are not either of those.
- Every placement question answerable from the layering rather than from precedent.
- The external seam does not move. `pmm.hpp` stays exactly as it is, so none of the ~180 call sites change and the work is verifiable by an unchanged boot at every step.
- Each step behaviour-neutral and separately committable, with the watermark gate landing last, as a consequence rather than a cause.

Non-goals:

- Removing zones. They are a deliberate Linux-derived exception and the reason was reaffirmed on 2026-10-28: they exist to serve drivers with addressing constraints, and the absence of such a driver today is a gap in the project, not a property of the design.
- The reclaimer and its wake signal. This makes room for them; it does not build them.
- The zone walk's order (`docs/repairs/261028/mm.md` item 1). The walk becomes one object's job here, which is what makes that fixable in one place afterwards.
- Changing what any allocation currently returns. Every step is meant to be invisible.
- `PageQueues` moving. Reclaim ranking is per-node in both references (Zircon's `PmmNode` owns its `PageQueues`; Linux's `pg_data_t` owns the LRU), and the reclaimer will be woken per node. It stays on `PmNode`.

## Design

### The four layers

**`FrameBuddy`** — the free-list mechanism for one pfn span: split, merge, take, give back. Already exists and is already the right shape. `FrameSet` disappears into it or into `PmZone`: apply the deletion test and nothing reappears, because its five methods have one user and three of them forward.

**`PmZone`** — one contiguous run of frames of one `ZoneType`. Owns its counters, its watermarks, its lock and its buddy. Its interface answers two questions and nothing else: *can you serve this request at this order*, and *here is a block back*. The admission decision is evaluated here because it is a question about this zone's own free count and watermarks — but the **criterion** comes in with the request, which is what was missing before.

**`PmNode`** — a NUMA domain: its zones, the fallback order over them, its `PageQueues`, its CPU affinity. Its interface answers *serve this request from my zones or my neighbours'*. It stops being the registry, the topology and the per-CPU accessor.

**`AllocRequest`** — the one new name, replacing four. Built once per allocation from the caller's `Gaf`, the order, the nodemask, the thread's policy and the node's. Carries:

- `allowed_nodes`, `home_node`, `initiator` — what `MemoryAllocationContext` carries today;
- the zone walk, as `ZoneIterator` already does, so the filter exists once;
- the derived admission criterion — which watermark this request is held to — with a real value rather than a permanently empty `AllocationOptions`.

`MemoryAllocationContext`, `AllocationOptions`, `gaf_to_allocation_control()` and the dead `AllocationContext` all fold into it. The type count over one allocation goes from four to one.

### Events, one allocation

```text
pmm.hpp free function
  -> PmNode::alloc_frames(gaf, order, nodemask)
       builds AllocRequest once   (policy applied here, not per zone)
       walks request.zones()      (one filter, in ZoneIterator)
         -> PmZone::alloc(request, order)
              admission: this zone's free count against request's criterion
              mechanism: FrameBuddy under the zone lock
       postamble: locality counters
```

The difference from today is not the shape of the call chain, which is roughly this already. It is that the policy is computed once, at the top, into a value every layer below reads instead of re-deriving — and that no layer below has to be told three things (`gaf`, `options`, `context`) that are three views of one thing.

### What leaves `PmNode`

**The registry.** Which nodes exist, which are online, which have memory, and how to find one by id. `pmm.hpp` already presents exactly this interface to the kernel — `node_online_mask()`, `node_possible_mask()`, `num_nodes()`, `current_node()` — so only the storage moves, from static members of `PmNode` to file scope behind those functions. Linux's `node_data[]` and `node_states[]` are the same arrangement.

**The topology.** The distance matrix and the two CPU arrays, fed by `kernel/arch/x86/mm/srat.cpp`. This is the one part of the cluster with a genuine second user, so it is the one part that should have its own header: it is an arch input, not a property of any node. Linux keeps it in `arch/x86/mm/numa.c` for the same reason.

**The per-CPU current node.** `PmNode::Current` is a `CpuLocal::PerCpu<PmNode *>` plus `apply_thread_policy()`, which turns (thread state, `Gaf`) into a node. The first half is per-CPU state that `CpuLocal` already models. The second half is request construction, and belongs with `AllocRequest`, which is the thing that needs the answer.

**`Router`.** An algorithm over the topology and the registry that produces the fallback order once at boot. It reads node state and distances and writes nothing of the node's, so it is a bring-up function, not node state.

### Depth, measured

| Module | Public interface today | Real callers today | After |
|---|---|---|---|
| `pmm.hpp` free functions | ~30 functions | ~180 call sites across the kernel | unchanged; this is the seam and it is already deep |
| `PmNode` | ~25 methods, 3 nested classes | `pmm.cpp`, `srat.cpp` (4 calls), 2 tests | ~6 methods plus bring-up |
| `PmZone` | ~20 methods | `PmNode`, 1 test | ~8 methods |
| request types | 4 types, one dead | — | 1 type |

The seam is in the right place already, which is the good news and the reason this is affordable: everything above is behind `pmm.hpp`, so the kernel outside `kernel/mm/` cannot tell the difference at any step.

### Concurrency

Nothing about the locking changes, and that is deliberate. The zone lock keeps covering the buddy and nothing else; the counters stay atomics read without it; the watermarks stay write-once-at-boot. `AllocRequest` is a stack value owned by one thread for the length of one allocation, so it needs no synchronisation at all — which is itself an argument for it existing, because the policy it carries is currently re-derived inside loops that run under different locks.

The one place this design touches concurrency is the admission state the gate will later set (`Flags::BelowHighWmark`), and that is deliberately left to the gate's own change, with its own argument, once the gate has a principled home.

## Alternatives considered

**Fix only the request object and leave the three classes alone.** This was proposed and rejected on 2026-10-28. It is also insufficient on its own: `PmNode` would still be seven things, so the next placement question — where does the free-memory signal live, where does the reclaimer hang — gets the same non-answer that produced the rejected gate design.

**Go full Zircon: delete zones, keep one `PmmNode` per machine over passive arenas.** It is the smallest layering and the reference's own. Rejected: it throws away the addressing-constraint machinery that is the whole reason zones were taken from Linux, and the user reaffirmed that reason while this was being designed.

**Keep `PmNode::Topology` and `PmNode::Current` as nested classes and only move the registry.** Rejected: the nesting is what makes `srat.cpp` — an arch file — reach into a mm class to set a distance matrix. Nesting expresses ownership, and the node does not own the machine's topology.

**Split further: a separate class for the fallback order, another for the counters, another for admission policy.** Rejected: the user has asked for methods that are closed over what they need, not for more objects to wire together. Four layers plus a request value is already the point at which each has one job; past that, depth falls because the interfaces multiply faster than the behaviour behind them.

**Make the registry and topology a `Pmm` singleton object, as Zircon does.** Genuinely open; see D2. It would give the bring-up order an explicit owner, at the cost of a singleton and an indirection on paths that are currently direct static reads.

**Leave the dead state in place and design around it.** Rejected as a default, but it is not mine to decide: the repo's rule is that an empty class or unreachable stub is usually scaffolding for planned work. Hence D4.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the admission criterion is evaluated | (a) `PmZone::alloc(request, order)` asks the request for its criterion and tests its own counters; (b) `AllocRequest::admits(zone, order)` reads the zone's counters and decides | (a) — the free count and watermarks are the zone's, and (b) would have the request reach into zone state it does not own; the request supplies the criterion, the zone applies it | None (homework paused) | |
| D2. Registry and topology storage | (a) file-scope tables behind the existing `pmm.hpp` functions, as Linux does; (b) a `Pmm` singleton that owns the registry, the topology and the nodes, as Zircon does | (a) — `pmm.hpp` is already that interface, so only storage moves and no call site changes; (b) adds an object whose only job is to hold things | None | |
| D3. What becomes of `PmNode::Current` | (a) dissolves: the `PerCpu<PmNode *>` becomes an ordinary `CpuLocal` variable and `apply_thread_policy()` becomes part of building an `AllocRequest`; (b) survives as a small class over per-CPU node state only, with the policy half moving | (a) — its two halves answer different questions and only one is per-CPU state | None | |
| D4. The dead state: `PmZone::m_frames`, the per-CPU frame cache, `PmNode`'s three counters, `FrameSet` | per item: delete, or keep as scaffolding | delete `m_frames` (a pure duplicate of the buddy's lists, no reader); delete `FrameSet` (fails the deletion test); **keep** the per-CPU frame cache (it is real planned work and the order limit is how it is switched off); **keep** `PmNode`'s counters but give them `dump()` as a caller, or delete them with `dump()`'s zone half intact | None | |
| D5. Migration shape | (a) a sequence of behaviour-neutral steps, each committed and boot-verified, request object first; (b) one change | (a), and the order is in the plan this design feeds; the watermark gate lands last, as a consequence | None | |
| D6. Whether `PmZone::alloc_frames()`'s `MemoryAllocationContext` parameter is dead rather than unused | (a) the request replaces it and the question disappears; (b) investigate first | (a) — the walk already filters on nodes before calling in, so the zone needs nothing from it; the request makes this concrete rather than arguable | None | |

## Testing and verification

Every step is meant to be invisible from outside `kernel/mm/`, so the standing check is `ours-verify` on both images with **identical** test counts to the step before — currently `dbg 44 passed, 4 skipped` and `rel 41 passed, 8 skipped, 6/6 required`, with `host-tests 148 passed`. A changed count at any step means that step was not behaviour-neutral, which is the whole signal being bought by doing this in sequence.

Two things get real tests rather than only a no-regression boot:

- `AllocRequest`'s construction is pure — `(Gaf, order, nodemask, policy)` in, a value out — so it is the first part of this cluster that a host test can reach, in the shape of `kernel/mm/tests/frame_buddy_smoke.cpp`. The cases worth pinning are the ones that are currently spread across `alloc_frames_preamble()` and `apply_thread_policy()`: `ThisNode` narrowing only when the caller's mask includes the local node, a mask that excludes the local node keeping its own nodes, and a memoryless node not being narrowed to.
- `kernel/commands/pm_node_test.cpp` already tests `ZoneIterator` and `AllocationContext` against nothing. Once the walk is the request's, those cases become tests of the production path without being rewritten, which is the one piece of luck in this cluster.

What cannot be verified: that the layering is better. Identical boots prove only that nothing broke. The claim being made is about where the next change can go, and the test of it is whether the watermark gate, the free-memory signal and the reclaimer each land in one obvious place afterwards — which is a judgement to make at that point, not a check to run now.

## Open questions

- Whether `Flags::Contiguous` on `PmZone` has an intended reader. It has none today, and `ZoneType::Dynamic` and `AllocationOptions::Contiguous` suggest a contiguous-allocation feature that was started in three places and finished in none. Worth establishing before the flag word is touched, because it changes whether that word has anything in it but the admission bit.
- Whether the per-CPU frame cache should be made real as part of this or left switched off. It is the one piece of dead state that is clearly planned rather than abandoned, and it is also the only thing in the cluster that would change the locking story — a cache that serves order-0 allocations without the zone lock is the reason the zone lock's granularity matters. Keeping it off is the safe default; making it real is a performance item and belongs to the user by the repo's own rule.
- Whether `pmm.hpp`'s free-function layer should shrink while the classes behind it are reshaped. It has four overloads of `alloc_frames` and four of `get_frames`, several differing only in defaults, and the `ok(nullptr)` bug of `4929d8a0` lived in exactly that layer. Not part of this design, because the seam not moving is what makes the rest affordable, but it is the obvious next question after it.
