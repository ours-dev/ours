# pmm-layering: give PmNode, PmZone and ZoneList one job each, and the allocation request a name

**Date**: 2026-10-28
**Status**: Accepted (D1, D2, D4, D7 decided 2026-10-28; D3 open, decided at step 6)

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

### The layers, and the role each name claims

Naming is load-bearing here, because the reason the admission check had nowhere to go is that the object whose job it is had no name. Each row below says what role the name claims and what the thing owns; where the claim and the code disagree today, that is said.

| Name | Role it claims | Owns | State |
|---|---|---|---|
| `FrameBuddy` | mechanism | the free lists for one pfn span | mutable, under the zone lock |
| `PmZone` | the source and the sink of frames for one span | counters, watermarks, lock, buddy | mutable |
| `ZoneList` | the candidate order | the by-type array and the fallback order | written at boot, read after |
| `ZoneSelector` | **selector**: which candidate | the walk cursor, and applying the request's criterion | mutable, one per allocation, on the stack |
| `AllocRequest` | the request and what was resolved from it | gaf, order, zone range, allowed nodes, home node, initiator, admission mark | **immutable**, one per allocation |
| `resolve_request()` | **resolver**: given these conditions, what should this allocation be | nothing | pure function |
| `PmNode` | a NUMA domain | its zones, its `ZoneList`, its `PageQueues`, its affinity | mutable |
| `NodeRegistry` | **registry**: which nodes exist, in what state | the node table and the state masks | written at boot |
| `NumaTopology` | the machine's CPU-to-node map | the distance matrix and the two CPU arrays | written by the arch layer at boot |
| `pmm.hpp` | the seam | nothing | — |

Two names in the tree today claim a role they do not play, and both misnamings are informative.

`PmNode::Router` claims runtime dispatch — a router sends work to a destination. It does not: `Router::build()` computes an ordering once at boot, the result is stored in `ZoneList::m_fallback`, and the object takes no part in any allocation afterwards. It is an order-builder, and calling it a router is part of why the thing that *does* choose at runtime was never recognised as an object.

That thing is `ZoneSelector`, and it does not exist. Choosing a zone per allocation lives inside a lambda in `get_frame_from_zone_list()`. **This is the whole reason the watermark admission check had no principled home**: deciding whether a candidate qualifies is a selector's job, and there was no selector. The rejected gate design parked the check inside `PmZone` — the object that owns the free lists — which made the zone simultaneously the policy and the mechanism, and then needed a reason invented for it.

### Why the request is one value and not two

The first draft of this design had `AllocRequest` carry the caller's ask, the derived policy **and** the walk cursor. That is the same mistake as `PmNode` being seven things, at a smaller scale, and it is the shape `AllocationContext` already has: it holds a `ZoneIterator` whose `next` index is mutable inside a value that is otherwise read-only.

Applying the deletion test to the split rather than guessing: separate the derived policy into its own type and nothing reappears — its fields have the same lifetime, the same owner and the same immutability as the caller's ask, so they are one value's worth of data. Separate the **cursor** and something real changes: a mutable iteration position stops living inside an immutable value, and the loop that advances it gets a name.

So: the data fuses, the cursor splits. `AllocRequest` is immutable and copyable; `ZoneSelector` holds the cursor and borrows the request.

### Zone constraint belongs in the request

`AllocationContext::build()` hard-codes the same range for every allocation:

```cpp
// A Gaf names no zone, so every allocation may use zones up to
// Normal. It used to be read from Gaf's Dma, Dma32 and Movable bits,
// which nothing ever passed.
ziter.set_nodes(nodemask).set_zone_range(ZoneType::Dma, ZoneType::Normal);
```

Read as an unfinished feature, that is a TODO. Read as an interface, it is an error: it lets every caller treat zones as interchangeable, and zones exist here precisely because they are not — a Dma zone's frames are addressable by a constrained device and a Normal zone's are not. An abstraction may unify an interface; it may not pretend the difference away.

This is not a new field. `ZoneIterator` already has `lower_zone_type` and `upper_zone_type`, and `move_next()` reads only the upper one (`docs/repairs/261028/mm.md` item 2). The change is that both move onto the request, both get read, and the all-zones range becomes a visible default rather than a law. No caller has to name a zone yet; the point is that one can, and that the default stops being invisible.

### Events, one allocation

```text
pmm.hpp free function
  -> resolve_request(gaf, order, nodemask, thread policy)  -> AllocRequest   (immutable)
  -> PmNode::alloc_frames(request)
       ZoneSelector over this node's ZoneList and the request
         for each candidate zone:
           node filter      (request.allowed_nodes)      one place, not two
           zone-type filter (request.zone_range)         both bounds read
           admission        (zone.watermark_ok(order, request.admit_at))
         -> PmZone::alloc(gaf, order)     pure mechanism: buddy under the lock
       postamble: locality counters
```

`PmZone::alloc()` loses its gate entirely and becomes mechanism only. The zone still owns the predicate — `watermark_ok()` is a question about the zone's own counters and only the zone can answer it — but the selector owns the loop and decides which mark to apply. That is the split the rejected design could not express.

### What leaves `PmNode`

**The registry.** Which nodes exist, which are online, which have memory, and how to find one by id. `pmm.hpp` already presents exactly this interface to the kernel — `node_online_mask()`, `node_possible_mask()`, `num_nodes()`, `current_node()` — so only the storage moves, from static members of `PmNode` to file scope behind those functions. Linux's `node_data[]` and `node_states[]` are the same arrangement.

**The topology.** The distance matrix and the two CPU arrays, fed by `kernel/arch/x86/mm/srat.cpp`. This is the one part of the cluster with a genuine second user, so it is the one part that should have its own header: it is an arch input, not a property of any node. Linux keeps it in `arch/x86/mm/numa.c` for the same reason.

**The per-CPU current node.** `PmNode::Current` is a `CpuLocal::PerCpu<PmNode *>` plus `apply_thread_policy()`, which turns (thread state, `Gaf`) into a node. The first half is per-CPU state that `CpuLocal` already models. The second half is request construction, and belongs with `AllocRequest`, which is the thing that needs the answer.

**`Router`.** An algorithm over the topology and the registry that produces the fallback order once at boot. It reads node state and distances and writes nothing of the node's, so it is a bring-up function, not node state. It also loses the name: see the role table above.

**Why this is not merely moving code.** A suffix does not decouple anything; the dependency edges decide. So: of the five files outside `pm_node.cpp`/`pm_zone.cpp` that include either header, what does each actually use?

| File | What it uses |
|---|---|
| `kernel/mm/cpu_local.cpp` | `PmNode::Current`, nothing else |
| `kernel/mm/object_cache.cpp` | `PmNode::State`, `for_each_nid` — the registry |
| `kernel/mm/memmap_init.cpp` | `PmNode::node` — the registry — and one `PmZone` counter |
| `kernel/mm/sparse_memmap.cpp` | **nothing**; the include is dead, as is the one in `priv/memmap.hpp` |
| `kernel/arch/x86/mm/srat.cpp` | `State`, `set_state`, `Topology` |

**Not one of the five allocates.** Every one of them wants the registry, the topology or the per-CPU current node — exactly the three things leaving `PmNode` — and one include is dead. So the extraction removes the allocator's header from five translation units that have no business seeing it, which is a change in the dependency graph and not a rename.

### Depth, measured

| Module | Public interface today | Real callers today | After |
|---|---|---|---|
| `pmm.hpp` free functions | ~30 functions | ~180 call sites across the kernel | unchanged; this is the seam and it is already deep |
| `PmNode` | ~25 methods, 3 nested classes | `pmm.cpp`, `srat.cpp` (4 calls), 2 tests | ~6 methods plus bring-up |
| `PmZone` | ~20 methods | `PmNode`, 1 test | ~8 methods |
| request types | 4 types, one dead | — | 1 immutable value plus 1 stack selector |

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
| D1. Where the admission criterion is applied | (a) `PmZone::alloc()` asks the request for its mark and tests its own counters; (b) `AllocRequest::admits(zone, order)`; (c) **`ZoneSelector`** applies it while choosing a candidate, and `PmZone::alloc()` becomes pure mechanism | **(c)**. This option did not exist in the first draft, and its absence is why the rejected gate design had to park the check inside the object that owns the free lists. Deciding whether a candidate qualifies is a selector's job. The zone keeps the predicate, because `watermark_ok()` is a question only the zone can answer about itself; the selector keeps the loop and the choice of mark | None (homework paused) | **(c)**, 2026-10-28 |
| D2. Registry and topology storage | (a) file-scope tables behind the existing `pmm.hpp` functions; (b) a `Pmm` singleton owning the registry, the topology and the nodes | (a). Not on the reference's authority but on the dependency table above: five translation units include the allocator's header for things that are not allocation, and (a) removes that edge while (b) adds an object whose only job is to hold things | None | **(a)**, 2026-10-28 |
| D3. What becomes of `PmNode::Current` | (a) dissolves: the `PerCpu<PmNode *>` becomes an ordinary `CpuLocal` variable and `apply_thread_policy()` becomes part of `resolve_request()`; (b) survives as a small class over per-CPU node state only | (a). Its two halves answer different questions and only one of them is per-CPU state | None | |
| D4. The dead state, by the deletion test | per item: delete, or keep | `PmZone::m_frames`: **delete** — zero readers, a pure duplicate of the buddy's lists, nothing reappears. `FrameSet`: **delete** — five methods, three of them one-line forwards, one user; its two loops move onto the buddy and nothing reappears. `PmNode::m_present_frames` and `m_reserved_frames`: **delete**, with `add_reserved_frames()`/`sub_reserved_frames()`, their two call sites and `dump()`'s node half — they duplicate the sum of the zones' own and only an uncalled function reads them. `PmNode::m_spanned_frames`: **becomes a local** — correcting the first draft, which claimed `dump()` was its only reader; `pm_node_init.cpp:268` reads it one line after writing it, to decide whether the node gets `State::Memory`, and that state is what `Router::build()` walks. The member is dead after `init()`, the value is not. The per-CPU frame cache: **keep**, and note that the deletion test does not settle it — nothing reappears across callers because it has none, so what argues for keeping it is the repo's rule about scaffolding for planned work, not depth | None | **as recommended**, 2026-10-28 |
| D5. Migration shape | (a) a sequence of behaviour-neutral steps, each committed and boot-verified; (b) one change | (a), with `AllocRequest` and `ZoneSelector` first, because every later step reads better once the request has a name; the watermark gate lands last, as a consequence | None | |
| D6. `PmZone::alloc_frames()`'s unused `MemoryAllocationContext` parameter | (a) the selector replaces it and the question disappears; (b) investigate first | (a). Under D1(c) the zone is handed a `Gaf` and an order and nothing else, so the parameter does not survive to be dead or alive | None | resolved by D1(c) |
| D7. Whether the zone-type range joins the request now | (a) yes: both of `ZoneIterator`'s existing bounds move onto the request and both get read; (b) later, when a caller needs to name a zone | (a). It is not a new field — the bounds exist and `move_next()` reads only the upper one — and leaving the all-zones range hard-coded is the interface pretending zones are interchangeable | None | **(a)**, 2026-10-28, on the recommendation, flagged before step 3 began rather than chosen separately |

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
